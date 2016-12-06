/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2016 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/service-request-handlers.h"

#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/jit/tc-record.h"

#include "hphp/runtime/vm/jit/func-guard.h"
#include "hphp/runtime/vm/jit/perf-counters.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/stub-alloc.h"
#include "hphp/runtime/vm/jit/smashable-instr.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/unwind-itanium.h"

#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/treadmill.h"

#include "hphp/ppc64-asm/decoded-instr-ppc64.h"
#include "hphp/vixl/a64/decoder-a64.h"

#include "hphp/util/arch.h"
#include "hphp/util/ringbuffer.h"
#include "hphp/util/trace.h"

TRACE_SET_MOD(mcg);

namespace HPHP { namespace jit { namespace tc {

TCA bindJmp(TCA toSmash, SrcKey destSk, TransFlags trflags, bool& smashed) {
  auto const sr = srcDB().find(destSk);
  always_assert(sr);
  auto const tDest = sr->getTopTranslation();
  if (tDest == nullptr) return nullptr;

  auto codeLock = lockCode();

  auto const isJcc = [&] {
    switch (arch()) {
      case Arch::X64: {
        x64::DecodedInstruction di(toSmash);
        return (di.isBranch() && !di.isJmp());
      }

      case Arch::ARM: {
        auto instr = reinterpret_cast<vixl::Instruction*>(toSmash);
        return instr->IsCondBranchImm();
      }

      case Arch::PPC64:
        ppc64_asm::DecodedInstruction di(toSmash);
        return di.isBranch(ppc64_asm::AllowCond::OnlyCond);
    }
    not_reached();
  }();

  if (isJcc) {
    auto const target = smashableJccTarget(toSmash);
    assertx(target);

    // Return if already smashed.
    if (target == tDest) return tDest;
    sr->chainFrom(IncomingBranch::jccFrom(toSmash));
  } else {
    auto const target = smashableJmpTarget(toSmash);
    assertx(target);

    // Return if already smashed.
    if (!target || target == tDest) return tDest;
    sr->chainFrom(IncomingBranch::jmpFrom(toSmash));
  }

  smashed = true;
  return tDest;
}

TCA bindAddr(TCA toSmash, SrcKey destSk, TransFlags trflags, bool& smashed) {
  auto const sr = srcDB().find(destSk);
  always_assert(sr);
  auto const tDest = sr->getTopTranslation();
  if (tDest == nullptr) return nullptr;

  auto codeLock = lockCode();

  auto addr = reinterpret_cast<TCA*>(toSmash);
  if (*addr == tDest) {
    // Already smashed
    return tDest;
  }
  sr->chainFrom(IncomingBranch::addr(addr));
  smashed = true;
  return tDest;
}

void bindCall(TCA toSmash, TCA start, Func* callee, int nArgs, bool immutable) {
  auto codeLock = lockCode();

  if (!start || smashableCallTarget(toSmash) == start) return;
  assertx(smashableCallTarget(toSmash));
  TRACE(2, "bindCall smash %p -> %p\n", toSmash, start);
  smashCall(toSmash, start);

  bool is_profiled = false;
  // For functions to be PGO'ed, if their current prologues are still
  // profiling ones (living in code.prof()), then save toSmash as a
  // caller to the prologue, so that it can later be smashed to call a
  // new prologue when it's generated.
  int calleeNumParams = callee->numNonVariadicParams();
  int calledPrologNumArgs = (nArgs <= calleeNumParams ?
                             nArgs :  calleeNumParams + 1);
  auto const profData = jit::profData();
  if (profData != nullptr && code().prof().contains(start)) {
    auto rec = profData->prologueTransRec(
      callee,
      calledPrologNumArgs
    );
    if (immutable) {
      rec->addMainCaller(toSmash);
    } else {
      rec->addGuardCaller(toSmash);
    }
    is_profiled = true;
  }

  // We need to be able to reclaim the function prologues once the unit
  // associated with this function is treadmilled-- so record all of the
  // callers that will need to be re-smashed
  //
  // Additionally for profiled calls we need to remove them from the main
  // and guard caller maps.
  if (RuntimeOption::EvalEnableReusableTC) {
    if (debug || is_profiled || !immutable) {
      auto metaLock = lockMetadata();
      recordFuncCaller(callee, toSmash, immutable, is_profiled,
                           calledPrologNumArgs);
    }
  }
}

}}}
