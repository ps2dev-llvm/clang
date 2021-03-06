//===- AnalyzerOptions.cpp - Analysis Engine Options ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains special accessors for analyzer configuration options
// with string representations.
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Core/AnalyzerOptions.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

using namespace clang;
using namespace ento;
using namespace llvm;

std::vector<StringRef>
AnalyzerOptions::getRegisteredCheckers(bool IncludeExperimental /* = false */) {
  static const StringRef StaticAnalyzerChecks[] = {
#define GET_CHECKERS
#define CHECKER(FULLNAME, CLASS, DESCFILE, HELPTEXT, GROUPINDEX, HIDDEN)       \
  FULLNAME,
#include "clang/StaticAnalyzer/Checkers/Checkers.inc"
#undef CHECKER
#undef GET_CHECKERS
  };
  std::vector<StringRef> Result;
  for (StringRef CheckName : StaticAnalyzerChecks) {
    if (!CheckName.startswith("debug.") &&
        (IncludeExperimental || !CheckName.startswith("alpha.")))
      Result.push_back(CheckName);
  }
  return Result;
}

AnalyzerOptions::UserModeKind AnalyzerOptions::getUserMode() {
  if (!UserMode.hasValue()) {
    StringRef ModeStr = getOptionAsString("mode", "deep");
    UserMode = llvm::StringSwitch<llvm::Optional<UserModeKind>>(ModeStr)
      .Case("shallow", UMK_Shallow)
      .Case("deep", UMK_Deep)
      .Default(None);
    assert(UserMode.getValue() && "User mode is invalid.");
  }
  return UserMode.getValue();
}

AnalyzerOptions::ExplorationStrategyKind
AnalyzerOptions::getExplorationStrategy() {
  if (!ExplorationStrategy.hasValue()) {
    StringRef StratStr = getOptionAsString("exploration_strategy",
                                           "unexplored_first_queue");
    ExplorationStrategy =
        llvm::StringSwitch<llvm::Optional<ExplorationStrategyKind>>(StratStr)
            .Case("dfs", ExplorationStrategyKind::DFS)
            .Case("bfs", ExplorationStrategyKind::BFS)
            .Case("unexplored_first",
                  ExplorationStrategyKind::UnexploredFirst)
            .Case("unexplored_first_queue",
                  ExplorationStrategyKind::UnexploredFirstQueue)
            .Case("unexplored_first_location_queue",
                  ExplorationStrategyKind::UnexploredFirstLocationQueue)
            .Case("bfs_block_dfs_contents",
                  ExplorationStrategyKind::BFSBlockDFSContents)
            .Default(None);
    assert(ExplorationStrategy.hasValue() &&
           "User mode is invalid.");
  }
  return ExplorationStrategy.getValue();
}

IPAKind AnalyzerOptions::getIPAMode() {
  if (!IPAMode.hasValue()) {
    // Use the User Mode to set the default IPA value.
    // Note, we have to add the string to the Config map for the ConfigDumper
    // checker to function properly.
    const char *DefaultIPA = nullptr;
    UserModeKind HighLevelMode = getUserMode();
    if (HighLevelMode == UMK_Shallow)
      DefaultIPA = "inlining";
    else if (HighLevelMode == UMK_Deep)
      DefaultIPA = "dynamic-bifurcate";
    assert(DefaultIPA);

    // Lookup the ipa configuration option, use the default from User Mode.
    StringRef ModeStr = getOptionAsString("ipa", DefaultIPA);
    IPAMode = llvm::StringSwitch<llvm::Optional<IPAKind>>(ModeStr)
            .Case("none", IPAK_None)
            .Case("basic-inlining", IPAK_BasicInlining)
            .Case("inlining", IPAK_Inlining)
            .Case("dynamic", IPAK_DynamicDispatch)
            .Case("dynamic-bifurcate", IPAK_DynamicDispatchBifurcate)
            .Default(None);
    assert(IPAMode.hasValue() && "IPA Mode is invalid.");
  }

  return IPAMode.getValue();
}

bool
AnalyzerOptions::mayInlineCXXMemberFunction(CXXInlineableMemberKind K) {
  if (getIPAMode() < IPAK_Inlining)
    return false;

  if (!CXXMemberInliningMode) {
    StringRef ModeStr = getOptionAsString("c++-inlining", "destructors");

    CXXMemberInliningMode =
      llvm::StringSwitch<llvm::Optional<CXXInlineableMemberKind>>(ModeStr)
      .Case("constructors", CIMK_Constructors)
      .Case("destructors", CIMK_Destructors)
      .Case("methods", CIMK_MemberFunctions)
      .Case("none", CIMK_None)
      .Default(None);

    assert(CXXMemberInliningMode.hasValue() &&
           "Invalid c++ member function inlining mode.");
  }

  return *CXXMemberInliningMode >= K;
}

static StringRef toString(bool b) { return b ? "true" : "false"; }

StringRef AnalyzerOptions::getCheckerOption(StringRef CheckerName,
                                            StringRef OptionName,
                                            StringRef Default,
                                            bool SearchInParents) {
  // Search for a package option if the option for the checker is not specified
  // and search in parents is enabled.
  ConfigTable::const_iterator E = Config.end();
  do {
    ConfigTable::const_iterator I =
        Config.find((Twine(CheckerName) + ":" + OptionName).str());
    if (I != E)
      return StringRef(I->getValue());
    size_t Pos = CheckerName.rfind('.');
    if (Pos == StringRef::npos)
      return Default;
    CheckerName = CheckerName.substr(0, Pos);
  } while (!CheckerName.empty() && SearchInParents);
  return Default;
}

bool AnalyzerOptions::getBooleanOption(StringRef Name, bool DefaultVal,
                                       const CheckerBase *C,
                                       bool SearchInParents) {
  // FIXME: We should emit a warning here if the value is something other than
  // "true", "false", or the empty string (meaning the default value),
  // but the AnalyzerOptions doesn't have access to a diagnostic engine.
  StringRef Default = toString(DefaultVal);
  StringRef V =
      C ? getCheckerOption(C->getTagDescription(), Name, Default,
                           SearchInParents)
        : getOptionAsString(Name, Default);
  return llvm::StringSwitch<bool>(V)
      .Case("true", true)
      .Case("false", false)
      .Default(DefaultVal);
}

bool AnalyzerOptions::getBooleanOption(Optional<bool> &V, StringRef Name,
                                       bool DefaultVal, const CheckerBase *C,
                                       bool SearchInParents) {
  if (!V.hasValue())
    V = getBooleanOption(Name, DefaultVal, C, SearchInParents);
  return V.getValue();
}

bool AnalyzerOptions::includeTemporaryDtorsInCFG() {
  return getBooleanOption(IncludeTemporaryDtorsInCFG,
                          "cfg-temporary-dtors",
                          /* Default = */ true);
}

bool AnalyzerOptions::includeImplicitDtorsInCFG() {
  return getBooleanOption(IncludeImplicitDtorsInCFG,
                          "cfg-implicit-dtors",
                          /* Default = */ true);
}

bool AnalyzerOptions::includeLifetimeInCFG() {
  return getBooleanOption(IncludeLifetimeInCFG, "cfg-lifetime",
                          /* Default = */ false);
}

bool AnalyzerOptions::includeLoopExitInCFG() {
  return getBooleanOption(IncludeLoopExitInCFG, "cfg-loopexit",
                          /* Default = */ false);
}

bool AnalyzerOptions::includeRichConstructorsInCFG() {
  return getBooleanOption(IncludeRichConstructorsInCFG,
                          "cfg-rich-constructors",
                          /* Default = */ true);
}

bool AnalyzerOptions::includeScopesInCFG() {
  return getBooleanOption(IncludeScopesInCFG,
                          "cfg-scopes",
                          /* Default = */ false);
}

bool AnalyzerOptions::mayInlineCXXStandardLibrary() {
  return getBooleanOption(InlineCXXStandardLibrary,
                          "c++-stdlib-inlining",
                          /*Default=*/true);
}

bool AnalyzerOptions::mayInlineTemplateFunctions() {
  return getBooleanOption(InlineTemplateFunctions,
                          "c++-template-inlining",
                          /*Default=*/true);
}

bool AnalyzerOptions::mayInlineCXXAllocator() {
  return getBooleanOption(InlineCXXAllocator,
                          "c++-allocator-inlining",
                          /*Default=*/true);
}

bool AnalyzerOptions::mayInlineCXXContainerMethods() {
  return getBooleanOption(InlineCXXContainerMethods,
                          "c++-container-inlining",
                          /*Default=*/false);
}

bool AnalyzerOptions::mayInlineCXXSharedPtrDtor() {
  return getBooleanOption(InlineCXXSharedPtrDtor,
                          "c++-shared_ptr-inlining",
                          /*Default=*/false);
}

bool AnalyzerOptions::mayInlineCXXTemporaryDtors() {
  return getBooleanOption(InlineCXXTemporaryDtors,
                          "c++-temp-dtor-inlining",
                          /*Default=*/true);
}

bool AnalyzerOptions::mayInlineObjCMethod() {
  return getBooleanOption(ObjCInliningMode,
                          "objc-inlining",
                          /* Default = */ true);
}

bool AnalyzerOptions::shouldSuppressNullReturnPaths() {
  return getBooleanOption(SuppressNullReturnPaths,
                          "suppress-null-return-paths",
                          /* Default = */ true);
}

bool AnalyzerOptions::shouldAvoidSuppressingNullArgumentPaths() {
  return getBooleanOption(AvoidSuppressingNullArgumentPaths,
                          "avoid-suppressing-null-argument-paths",
                          /* Default = */ false);
}

bool AnalyzerOptions::shouldSuppressInlinedDefensiveChecks() {
  return getBooleanOption(SuppressInlinedDefensiveChecks,
                          "suppress-inlined-defensive-checks",
                          /* Default = */ true);
}

bool AnalyzerOptions::shouldSuppressFromCXXStandardLibrary() {
  return getBooleanOption(SuppressFromCXXStandardLibrary,
                          "suppress-c++-stdlib",
                          /* Default = */ true);
}

bool AnalyzerOptions::shouldCrosscheckWithZ3() {
  return getBooleanOption(CrosscheckWithZ3,
                          "crosscheck-with-z3",
                          /* Default = */ false);
}

bool AnalyzerOptions::shouldReportIssuesInMainSourceFile() {
  return getBooleanOption(ReportIssuesInMainSourceFile,
                          "report-in-main-source-file",
                          /* Default = */ false);
}


bool AnalyzerOptions::shouldWriteStableReportFilename() {
  return getBooleanOption(StableReportFilename,
                          "stable-report-filename",
                          /* Default = */ false);
}

bool AnalyzerOptions::shouldSerializeStats() {
  return getBooleanOption(SerializeStats,
                          "serialize-stats",
                          /* Default = */ false);
}

bool AnalyzerOptions::shouldElideConstructors() {
  return getBooleanOption(ElideConstructors,
                          "elide-constructors",
                          /* Default = */ true);
}

int AnalyzerOptions::getOptionAsInteger(StringRef Name, int DefaultVal,
                                        const CheckerBase *C,
                                        bool SearchInParents) {
  SmallString<10> StrBuf;
  llvm::raw_svector_ostream OS(StrBuf);
  OS << DefaultVal;

  StringRef V = C ? getCheckerOption(C->getTagDescription(), Name, OS.str(),
                                     SearchInParents)
                  : getOptionAsString(Name, OS.str());

  int Res = DefaultVal;
  bool b = V.getAsInteger(10, Res);
  assert(!b && "analyzer-config option should be numeric");
  (void)b;
  return Res;
}

unsigned AnalyzerOptions::getOptionAsUInt(Optional<unsigned> &V, StringRef Name,
                                          unsigned DefaultVal,
                                          const CheckerBase *C,
                                          bool SearchInParents) {
  if (!V.hasValue())
    V = getOptionAsInteger(Name, DefaultVal, C, SearchInParents);
  return V.getValue();
}

StringRef AnalyzerOptions::getOptionAsString(StringRef Name,
                                             StringRef DefaultVal,
                                             const CheckerBase *C,
                                             bool SearchInParents) {
  return C ? getCheckerOption(C->getTagDescription(), Name, DefaultVal,
                              SearchInParents)
           : StringRef(
                 Config.insert(std::make_pair(Name, DefaultVal)).first->second);
}

StringRef AnalyzerOptions::getOptionAsString(Optional<StringRef> &V,
                                             StringRef Name,
                                             StringRef DefaultVal,
                                             const ento::CheckerBase *C,
                                             bool SearchInParents) {
  if (!V.hasValue())
    V = getOptionAsString(Name, DefaultVal, C, SearchInParents);
  return V.getValue();
}

unsigned AnalyzerOptions::getAlwaysInlineSize() {
  if (!AlwaysInlineSize.hasValue())
    AlwaysInlineSize = getOptionAsInteger("ipa-always-inline-size", 3);
  return AlwaysInlineSize.getValue();
}

unsigned AnalyzerOptions::getMaxInlinableSize() {
  if (!MaxInlinableSize.hasValue()) {
    int DefaultValue = 0;
    UserModeKind HighLevelMode = getUserMode();
    switch (HighLevelMode) {
      case UMK_Shallow:
        DefaultValue = 4;
        break;
      case UMK_Deep:
        DefaultValue = 100;
        break;
    }

    MaxInlinableSize = getOptionAsInteger("max-inlinable-size", DefaultValue);
  }
  return MaxInlinableSize.getValue();
}

unsigned AnalyzerOptions::getGraphTrimInterval() {
  if (!GraphTrimInterval.hasValue())
    GraphTrimInterval = getOptionAsInteger("graph-trim-interval", 1000);
  return GraphTrimInterval.getValue();
}

unsigned AnalyzerOptions::getMaxSymbolComplexity() {
  if (!MaxSymbolComplexity.hasValue())
    MaxSymbolComplexity = getOptionAsInteger("max-symbol-complexity", 35);
  return MaxSymbolComplexity.getValue();
}

unsigned AnalyzerOptions::getMaxTimesInlineLarge() {
  if (!MaxTimesInlineLarge.hasValue())
    MaxTimesInlineLarge = getOptionAsInteger("max-times-inline-large", 32);
  return MaxTimesInlineLarge.getValue();
}

unsigned AnalyzerOptions::getMinCFGSizeTreatFunctionsAsLarge() {
  if (!MinCFGSizeTreatFunctionsAsLarge.hasValue())
    MinCFGSizeTreatFunctionsAsLarge = getOptionAsInteger(
      "min-cfg-size-treat-functions-as-large", 14);
  return MinCFGSizeTreatFunctionsAsLarge.getValue();
}

unsigned AnalyzerOptions::getMaxNodesPerTopLevelFunction() {
  if (!MaxNodesPerTopLevelFunction.hasValue()) {
    int DefaultValue = 0;
    UserModeKind HighLevelMode = getUserMode();
    switch (HighLevelMode) {
      case UMK_Shallow:
        DefaultValue = 75000;
        break;
      case UMK_Deep:
        DefaultValue = 225000;
        break;
    }
    MaxNodesPerTopLevelFunction = getOptionAsInteger("max-nodes", DefaultValue);
  }
  return MaxNodesPerTopLevelFunction.getValue();
}

bool AnalyzerOptions::shouldSynthesizeBodies() {
  return getBooleanOption("faux-bodies", true);
}

bool AnalyzerOptions::shouldPrunePaths() {
  return getBooleanOption("prune-paths", true);
}

bool AnalyzerOptions::shouldConditionalizeStaticInitializers() {
  return getBooleanOption("cfg-conditional-static-initializers", true);
}

bool AnalyzerOptions::shouldInlineLambdas() {
  if (!InlineLambdas.hasValue())
    InlineLambdas = getBooleanOption("inline-lambdas", /*Default=*/true);
  return InlineLambdas.getValue();
}

bool AnalyzerOptions::shouldWidenLoops() {
  if (!WidenLoops.hasValue())
    WidenLoops = getBooleanOption("widen-loops", /*Default=*/false);
  return WidenLoops.getValue();
}

bool AnalyzerOptions::shouldUnrollLoops() {
  if (!UnrollLoops.hasValue())
    UnrollLoops = getBooleanOption("unroll-loops", /*Default=*/false);
  return UnrollLoops.getValue();
}

bool AnalyzerOptions::shouldDisplayNotesAsEvents() {
  if (!DisplayNotesAsEvents.hasValue())
    DisplayNotesAsEvents =
        getBooleanOption("notes-as-events", /*Default=*/false);
  return DisplayNotesAsEvents.getValue();
}

bool AnalyzerOptions::shouldDisplayMacroExpansions() {
  if (!DisplayMacroExpansions.hasValue())
    DisplayMacroExpansions =
        getBooleanOption("expand-macros", /*Default=*/false);
  return DisplayMacroExpansions.getValue();
}

bool AnalyzerOptions::shouldAggressivelySimplifyBinaryOperation() {
  if (!AggressiveBinaryOperationSimplification.hasValue())
    AggressiveBinaryOperationSimplification =
      getBooleanOption("aggressive-binary-operation-simplification",
                       /*Default=*/false);
  return AggressiveBinaryOperationSimplification.getValue();
}

bool AnalyzerOptions::shouldEagerlyAssume() {
  if (!EagerlyAssumeBinOpBifurcation.hasValue())
    EagerlyAssumeBinOpBifurcation =
        getBooleanOption("eagerly-assume", true);
  return EagerlyAssumeBinOpBifurcation.getValue();
}

StringRef AnalyzerOptions::getCTUDir() {
  if (!CTUDir.hasValue()) {
    CTUDir = getOptionAsString("ctu-dir", "");
    if (!llvm::sys::fs::is_directory(*CTUDir))
      CTUDir = "";
  }
  return CTUDir.getValue();
}

bool AnalyzerOptions::naiveCTUEnabled() {
  if (!NaiveCTU.hasValue()) {
    NaiveCTU = getBooleanOption("experimental-enable-naive-ctu-analysis",
                                /*Default=*/false);
  }
  return NaiveCTU.getValue();
}

StringRef AnalyzerOptions::getCTUIndexName() {
  if (!CTUIndexName.hasValue())
    CTUIndexName = getOptionAsString("ctu-index-name", "externalFnMap.txt");
  return CTUIndexName.getValue();
}
