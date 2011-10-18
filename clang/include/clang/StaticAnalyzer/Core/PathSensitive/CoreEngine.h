//==- CoreEngine.h - Path-Sensitive Dataflow Engine ----------------*- C++ -*-//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines a generic engine for intraprocedural, path-sensitive,
//  dataflow analysis via graph reachability.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_GR_COREENGINE
#define LLVM_CLANG_GR_COREENGINE

#include "clang/AST/Expr.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExplodedGraph.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/WorkList.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/BlockCounter.h"
#include "llvm/ADT/OwningPtr.h"

namespace clang {

class ProgramPointTag;
  
namespace ento {

class NodeBuilder;

//===----------------------------------------------------------------------===//
/// CoreEngine - Implements the core logic of the graph-reachability
///   analysis. It traverses the CFG and generates the ExplodedGraph.
///   Program "states" are treated as opaque void pointers.
///   The template class CoreEngine (which subclasses CoreEngine)
///   provides the matching component to the engine that knows the actual types
///   for states.  Note that this engine only dispatches to transfer functions
///   at the statement and block-level.  The analyses themselves must implement
///   any transfer function logic and the sub-expression level (if any).
class CoreEngine {
  friend class CommonNodeBuilder;
  friend class NodeBuilder;
  friend class StmtNodeBuilder;
  friend class GenericNodeBuilderImpl;
  friend class BranchNodeBuilder;
  friend class IndirectGotoNodeBuilder;
  friend class SwitchNodeBuilder;
  friend class EndOfFunctionNodeBuilder;
  friend class CallEnterNodeBuilder;
  friend class CallExitNodeBuilder;

public:
  typedef std::vector<std::pair<BlockEdge, const ExplodedNode*> >
            BlocksExhausted;
  
  typedef std::vector<std::pair<const CFGBlock*, const ExplodedNode*> >
            BlocksAborted;

private:

  SubEngine& SubEng;

  /// G - The simulation graph.  Each node is a (location,state) pair.
  llvm::OwningPtr<ExplodedGraph> G;

  /// WList - A set of queued nodes that need to be processed by the
  ///  worklist algorithm.  It is up to the implementation of WList to decide
  ///  the order that nodes are processed.
  WorkList* WList;

  /// BCounterFactory - A factory object for created BlockCounter objects.
  ///   These are used to record for key nodes in the ExplodedGraph the
  ///   number of times different CFGBlocks have been visited along a path.
  BlockCounter::Factory BCounterFactory;

  /// The locations where we stopped doing work because we visited a location
  ///  too many times.
  BlocksExhausted blocksExhausted;
  
  /// The locations where we stopped because the engine aborted analysis,
  /// usually because it could not reason about something.
  BlocksAborted blocksAborted;

  void generateNode(const ProgramPoint &Loc,
                    const ProgramState *State,
                    ExplodedNode *Pred);

  void HandleBlockEdge(const BlockEdge &E, ExplodedNode *Pred);
  void HandleBlockEntrance(const BlockEntrance &E, ExplodedNode *Pred);
  void HandleBlockExit(const CFGBlock *B, ExplodedNode *Pred);
  void HandlePostStmt(const CFGBlock *B, unsigned StmtIdx, ExplodedNode *Pred);

  void HandleBranch(const Stmt *Cond, const Stmt *Term, const CFGBlock *B,
                    ExplodedNode *Pred);
  void HandleCallEnter(const CallEnter &L, const CFGBlock *Block,
                       unsigned Index, ExplodedNode *Pred);
  void HandleCallExit(const CallExit &L, ExplodedNode *Pred);

private:
  CoreEngine(const CoreEngine&); // Do not implement.
  CoreEngine& operator=(const CoreEngine&);

public:
  /// Construct a CoreEngine object to analyze the provided CFG using
  ///  a DFS exploration of the exploded graph.
  CoreEngine(SubEngine& subengine)
    : SubEng(subengine), G(new ExplodedGraph()),
      WList(WorkList::makeBFS()),
      BCounterFactory(G->getAllocator()) {}

  /// Construct a CoreEngine object to analyze the provided CFG and to
  ///  use the provided worklist object to execute the worklist algorithm.
  ///  The CoreEngine object assumes ownership of 'wlist'.
  CoreEngine(WorkList* wlist, SubEngine& subengine)
    : SubEng(subengine), G(new ExplodedGraph()), WList(wlist),
      BCounterFactory(G->getAllocator()) {}

  ~CoreEngine() {
    delete WList;
  }

  /// getGraph - Returns the exploded graph.
  ExplodedGraph& getGraph() { return *G.get(); }

  /// takeGraph - Returns the exploded graph.  Ownership of the graph is
  ///  transferred to the caller.
  ExplodedGraph* takeGraph() { return G.take(); }

  /// ExecuteWorkList - Run the worklist algorithm for a maximum number of
  ///  steps.  Returns true if there is still simulation state on the worklist.
  bool ExecuteWorkList(const LocationContext *L, unsigned Steps,
                       const ProgramState *InitState);
  void ExecuteWorkListWithInitialState(const LocationContext *L,
                                       unsigned Steps,
                                       const ProgramState *InitState, 
                                       ExplodedNodeSet &Dst);

  // Functions for external checking of whether we have unfinished work
  bool wasBlockAborted() const { return !blocksAborted.empty(); }
  bool wasBlocksExhausted() const { return !blocksExhausted.empty(); }
  bool hasWorkRemaining() const { return wasBlocksExhausted() || 
                                         WList->hasWork() || 
                                         wasBlockAborted(); }

  /// Inform the CoreEngine that a basic block was aborted because
  /// it could not be completely analyzed.
  void addAbortedBlock(const ExplodedNode *node, const CFGBlock *block) {
    blocksAborted.push_back(std::make_pair(block, node));
  }
  
  WorkList *getWorkList() const { return WList; }

  BlocksExhausted::const_iterator blocks_exhausted_begin() const {
    return blocksExhausted.begin();
  }
  BlocksExhausted::const_iterator blocks_exhausted_end() const {
    return blocksExhausted.end();
  }
  BlocksAborted::const_iterator blocks_aborted_begin() const {
    return blocksAborted.begin();
  }
  BlocksAborted::const_iterator blocks_aborted_end() const {
    return blocksAborted.end();
  }

  /// Enqueue the results of the node builder onto the work list.
  void enqueue(NodeBuilder &NB);
};

struct NodeBuilderContext {
  CoreEngine &Eng;
  const CFGBlock *Block;
  NodeBuilderContext(CoreEngine &E, const CFGBlock *B)
    : Eng(E), Block(B) { assert(B); }
};

/// This is the simplest builder which generates nodes in the ExplodedGraph.
class NodeBuilder {
protected:
  friend class StmtNodeBuilder;

  ExplodedNode *BuilderPred;
  const NodeBuilderContext &C;
  bool Finalized;

  /// \brief The frontier set - a set of nodes which need to be propagated after
  /// the builder dies.
  typedef llvm::SmallPtrSet<ExplodedNode*,5> DeferredTy;
  DeferredTy Deferred;

  BlockCounter getBlockCounter() const { return C.Eng.WList->getBlockCounter();}

  bool checkResults() {
    if (!Finalized)
      return false;
    for (DeferredTy::iterator I=Deferred.begin(), E=Deferred.end(); I!=E; ++I)
      if ((*I)->isSink())
        return false;
    return true;
  }

  virtual void finalizeResults() {
    if (!Finalized) {
      Finalized = true;
    }
  }
  
  ExplodedNode *generateNodeImpl(const ProgramPoint &PP,
                                 const ProgramState *State,
                                 ExplodedNode *Pred,
                                 bool MarkAsSink = false);

public:
  NodeBuilder(ExplodedNode *N, NodeBuilderContext &Ctx)
    : BuilderPred(N), C(Ctx), Finalized(false) {
    assert(!N->isSink());
    Deferred.insert(N);
  }

  /// Create a new builder using the parent builder's context.
  NodeBuilder(ExplodedNode *N, const NodeBuilder &ParentBldr)
    : BuilderPred(N), C(ParentBldr.C), Finalized(false) {
    assert(!N->isSink());
    Deferred.insert(N);
  }

  virtual ~NodeBuilder() {}

  /// \brief Generates a node in the ExplodedGraph.
  ///
  /// When a node is marked as sink, the exploration from the node is stopped -
  /// the node becomes the last node on the path.
  ExplodedNode *generateNode(const ProgramPoint &PP,
                             const ProgramState *State,
                             ExplodedNode *Pred,
                             bool MarkAsSink = false) {
    return generateNodeImpl(PP, State, Pred, MarkAsSink);
  }

  // \brief Get the builder's predecessor - the parent to all the other nodes.
  const ExplodedNode *getPred() const { return BuilderPred; }

  bool hasGeneratedNodes() const {
    return (!Deferred.count(BuilderPred));
  }

  typedef DeferredTy::iterator iterator;
  /// \brief Iterators through the results frontier.
  inline iterator results_begin() {
    finalizeResults();
    assert(checkResults());
    return Deferred.begin();
  }
  inline iterator results_end() {
    finalizeResults();
    return Deferred.end();
  }

  /// \brief Returns the number of times the current basic block has been
  /// visited on the exploded graph path.
  unsigned getCurrentBlockCount() const {
    return getBlockCounter().getNumVisited(
                         BuilderPred->getLocationContext()->getCurrentStackFrame(),
                         C.Block->getBlockID());
  }

  // \brief Get the builder's predecessor - the parent to all the other nodes.
  ExplodedNode *getPredecessor() const { return BuilderPred; }

  // \brief Returns state of the predecessor.
  const ProgramState *getState() const { return BuilderPred->getState(); }
};

class CommonNodeBuilder {
protected:
  ExplodedNode *Pred;
public:
  // TODO: make protected.
  CoreEngine& Eng;

  CommonNodeBuilder(CoreEngine* E, ExplodedNode *P) : Pred(P), Eng(*E) {}
  BlockCounter getBlockCounter() const { return Eng.WList->getBlockCounter(); }
};


class StmtNodeBuilder: public CommonNodeBuilder {
  const CFGBlock &B;
  const unsigned Idx;

public:
  bool PurgingDeadSymbols;
  bool BuildSinks;
  bool hasGeneratedNode;
  ProgramPoint::Kind PointKind;
  const ProgramPointTag *Tag;

  typedef llvm::SmallPtrSet<ExplodedNode*,5> DeferredTy;
  DeferredTy Deferred;

  void GenerateAutoTransition(ExplodedNode *N);

public:
  StmtNodeBuilder(const CFGBlock *b,
                  unsigned idx,
                  ExplodedNode *N,
                  CoreEngine* e);

  ~StmtNodeBuilder();

  ExplodedNode *getPredecessor() const { return Pred; }
  
  unsigned getCurrentBlockCount() const {
    return getBlockCounter().getNumVisited(
                            Pred->getLocationContext()->getCurrentStackFrame(),
                                           B.getBlockID());
  }

  ExplodedNode *generateNode(const Stmt *S,
                             const ProgramState *St,
                             ExplodedNode *Pred,
                             ProgramPoint::Kind K,
                             const ProgramPointTag *tag = 0) {
    hasGeneratedNode = true;

    if (PurgingDeadSymbols)
      K = ProgramPoint::PostPurgeDeadSymbolsKind;

    return generateNodeInternal(S, St, Pred, K, tag ? tag : Tag);
  }

  ExplodedNode *generateNode(const Stmt *S,
                             const ProgramState *St,
                             ExplodedNode *Pred,
                             const ProgramPointTag *tag = 0) {
    return generateNode(S, St, Pred, PointKind, tag);
  }

  ExplodedNode *generateNode(const ProgramPoint &PP,
                             const ProgramState *State,
                             ExplodedNode *Pred) {
    hasGeneratedNode = true;
    return generateNodeInternal(PP, State, Pred);
  }

  ExplodedNode*
  generateNodeInternal(const ProgramPoint &PP,
                       const ProgramState *State,
                       ExplodedNode *Pred);

  ExplodedNode*
  generateNodeInternal(const Stmt *S,
                       const ProgramState *State,
                       ExplodedNode *Pred,
                       ProgramPoint::Kind K,
                       const ProgramPointTag *tag = 0);

  /// getStmt - Return the current block-level expression associated with
  ///  this builder.
  const Stmt *getStmt() const { 
    const CFGStmt *CS = B[Idx].getAs<CFGStmt>();
    return CS ? CS->getStmt() : 0;
  }

  /// getBlock - Return the CFGBlock associated with the block-level expression
  ///  of this builder.
  const CFGBlock *getBlock() const { return &B; }

  unsigned getIndex() const { return Idx; }

  ExplodedNode *MakeNode(ExplodedNodeSet &Dst,
                         const Stmt *S, 
                         ExplodedNode *Pred,
                         const ProgramState *St) {
    return MakeNode(Dst, S, Pred, St, PointKind);
  }

  ExplodedNode *MakeNode(ExplodedNodeSet &Dst,
                         const Stmt *S,
                         ExplodedNode *Pred,
                         const ProgramState *St,
                         ProgramPoint::Kind K);

  ExplodedNode *MakeSinkNode(ExplodedNodeSet &Dst,
                             const Stmt *S,
                             ExplodedNode *Pred,
                             const ProgramState *St) {
    bool Tmp = BuildSinks;
    BuildSinks = true;
    ExplodedNode *N = MakeNode(Dst, S, Pred, St);
    BuildSinks = Tmp;
    return N;
  }

  void importNodesFromBuilder(const NodeBuilder &NB) {
    ExplodedNode *NBPred = const_cast<ExplodedNode*>(NB.getPred());
    if (NB.hasGeneratedNodes()) {
      Deferred.erase(NBPred);
      Deferred.insert(NB.Deferred.begin(), NB.Deferred.end());
      hasGeneratedNode = true;
    }
  }
};

class BranchNodeBuilder: public NodeBuilder {
  const CFGBlock *DstT;
  const CFGBlock *DstF;

  bool GeneratedTrue;
  bool GeneratedFalse;
  bool InFeasibleTrue;
  bool InFeasibleFalse;

  void finalizeResults() {
    if (Finalized)
      return;
    if (!GeneratedTrue) generateNode(BuilderPred->State, true);
    if (!GeneratedFalse) generateNode(BuilderPred->State, false);
    Finalized = true;
  }

public:
  BranchNodeBuilder(ExplodedNode *Pred, NodeBuilderContext &C,
                    const CFGBlock *dstT, const CFGBlock *dstF)
  : NodeBuilder(Pred, C), DstT(dstT), DstF(dstF),
    GeneratedTrue(false), GeneratedFalse(false),
    InFeasibleTrue(!DstT), InFeasibleFalse(!DstF) {
  }

  /// Create a new builder using the parent builder's context.
  BranchNodeBuilder(ExplodedNode *Pred, BranchNodeBuilder &ParentBldr)
  : NodeBuilder(Pred, ParentBldr), DstT(ParentBldr.DstT), DstF(ParentBldr.DstF),
    GeneratedTrue(false), GeneratedFalse(false),
    InFeasibleTrue(!DstT), InFeasibleFalse(!DstF) {
  }

  ExplodedNode *generateNode(const ProgramState *State, bool branch,
                             ExplodedNode *Pred = 0);

  const CFGBlock *getTargetBlock(bool branch) const {
    return branch ? DstT : DstF;
  }

  void markInfeasible(bool branch) {
    if (branch)
      InFeasibleTrue = GeneratedTrue = true;
    else
      InFeasibleFalse = GeneratedFalse = true;
  }

  bool isFeasible(bool branch) {
    return branch ? !InFeasibleTrue : !InFeasibleFalse;
  }
};

class IndirectGotoNodeBuilder {
  CoreEngine& Eng;
  const CFGBlock *Src;
  const CFGBlock &DispatchBlock;
  const Expr *E;
  ExplodedNode *Pred;

public:
  IndirectGotoNodeBuilder(ExplodedNode *pred, const CFGBlock *src, 
                    const Expr *e, const CFGBlock *dispatch, CoreEngine* eng)
    : Eng(*eng), Src(src), DispatchBlock(*dispatch), E(e), Pred(pred) {}

  class iterator {
    CFGBlock::const_succ_iterator I;

    friend class IndirectGotoNodeBuilder;
    iterator(CFGBlock::const_succ_iterator i) : I(i) {}
  public:

    iterator &operator++() { ++I; return *this; }
    bool operator!=(const iterator &X) const { return I != X.I; }

    const LabelDecl *getLabel() const {
      return llvm::cast<LabelStmt>((*I)->getLabel())->getDecl();
    }

    const CFGBlock *getBlock() const {
      return *I;
    }
  };

  iterator begin() { return iterator(DispatchBlock.succ_begin()); }
  iterator end() { return iterator(DispatchBlock.succ_end()); }

  ExplodedNode *generateNode(const iterator &I,
                             const ProgramState *State,
                             bool isSink = false);

  const Expr *getTarget() const { return E; }

  const ProgramState *getState() const { return Pred->State; }
};

class SwitchNodeBuilder {
  CoreEngine& Eng;
  const CFGBlock *Src;
  const Expr *Condition;
  ExplodedNode *Pred;

public:
  SwitchNodeBuilder(ExplodedNode *pred, const CFGBlock *src,
                    const Expr *condition, CoreEngine* eng)
  : Eng(*eng), Src(src), Condition(condition), Pred(pred) {}

  class iterator {
    CFGBlock::const_succ_reverse_iterator I;

    friend class SwitchNodeBuilder;
    iterator(CFGBlock::const_succ_reverse_iterator i) : I(i) {}

  public:
    iterator &operator++() { ++I; return *this; }
    bool operator!=(const iterator &X) const { return I != X.I; }
    bool operator==(const iterator &X) const { return I == X.I; }

    const CaseStmt *getCase() const {
      return llvm::cast<CaseStmt>((*I)->getLabel());
    }

    const CFGBlock *getBlock() const {
      return *I;
    }
  };

  iterator begin() { return iterator(Src->succ_rbegin()+1); }
  iterator end() { return iterator(Src->succ_rend()); }

  const SwitchStmt *getSwitch() const {
    return llvm::cast<SwitchStmt>(Src->getTerminator());
  }

  ExplodedNode *generateCaseStmtNode(const iterator &I,
                                     const ProgramState *State);

  ExplodedNode *generateDefaultCaseNode(const ProgramState *State,
                                        bool isSink = false);

  const Expr *getCondition() const { return Condition; }

  const ProgramState *getState() const { return Pred->State; }
};

class GenericNodeBuilderImpl {
protected:
  CoreEngine &engine;
  ExplodedNode *pred;
  ProgramPoint pp;
  SmallVector<ExplodedNode*, 2> sinksGenerated;  

  ExplodedNode *generateNodeImpl(const ProgramState *state,
                                 ExplodedNode *pred,
                                 ProgramPoint programPoint,
                                 bool asSink);

  GenericNodeBuilderImpl(CoreEngine &eng, ExplodedNode *pr, ProgramPoint p)
    : engine(eng), pred(pr), pp(p), hasGeneratedNode(false) {}

public:
  bool hasGeneratedNode;
  
  WorkList &getWorkList() { return *engine.WList; }
  
  ExplodedNode *getPredecessor() const { return pred; }
  
  BlockCounter getBlockCounter() const {
    return engine.WList->getBlockCounter();
  }
  
  const SmallVectorImpl<ExplodedNode*> &sinks() const {
    return sinksGenerated;
  }
};

template <typename PP_T>
class GenericNodeBuilder : public GenericNodeBuilderImpl {
public:
  GenericNodeBuilder(CoreEngine &eng, ExplodedNode *pr, const PP_T &p)
    : GenericNodeBuilderImpl(eng, pr, p) {}

  ExplodedNode *generateNode(const ProgramState *state, ExplodedNode *pred,
                             const ProgramPointTag *tag, bool asSink) {
    return generateNodeImpl(state, pred, cast<PP_T>(pp).withTag(tag),
                            asSink);
  }
  
  const PP_T &getProgramPoint() const { return cast<PP_T>(pp); }
};

class EndOfFunctionNodeBuilder : public CommonNodeBuilder {
  const CFGBlock &B;
  const ProgramPointTag *Tag;

public:
  bool hasGeneratedNode;

public:
  EndOfFunctionNodeBuilder(const CFGBlock *b, ExplodedNode *N, CoreEngine* e,
                           const ProgramPointTag *tag = 0)
    : CommonNodeBuilder(e, N), B(*b), Tag(tag), hasGeneratedNode(false) {}

  ~EndOfFunctionNodeBuilder();

  EndOfFunctionNodeBuilder withCheckerTag(const ProgramPointTag *tag) {
    return EndOfFunctionNodeBuilder(&B, Pred, &Eng, tag);
  }

  WorkList &getWorkList() { return *Eng.WList; }

  ExplodedNode *getPredecessor() const { return Pred; }

  unsigned getCurrentBlockCount() const {
    return getBlockCounter().getNumVisited(
                            Pred->getLocationContext()->getCurrentStackFrame(),
                                           B.getBlockID());
  }

  ExplodedNode *generateNode(const ProgramState *State,
                             ExplodedNode *P = 0,
                             const ProgramPointTag *tag = 0);

  void GenerateCallExitNode(const ProgramState *state);

  const CFGBlock *getBlock() const { return &B; }

  const ProgramState *getState() const {
    return getPredecessor()->getState();
  }
};

class CallEnterNodeBuilder {
  CoreEngine &Eng;

  const ExplodedNode *Pred;

  // The call site. For implicit automatic object dtor, this is the trigger 
  // statement.
  const Stmt *CE;

  // The context of the callee.
  const StackFrameContext *CalleeCtx;

  // The parent block of the CallExpr.
  const CFGBlock *Block;

  // The CFGBlock index of the CallExpr.
  unsigned Index;

public:
  CallEnterNodeBuilder(CoreEngine &eng, const ExplodedNode *pred, 
                         const Stmt *s, const StackFrameContext *callee, 
                         const CFGBlock *blk, unsigned idx)
    : Eng(eng), Pred(pred), CE(s), CalleeCtx(callee), Block(blk), Index(idx) {}

  const ProgramState *getState() const { return Pred->getState(); }

  const LocationContext *getLocationContext() const { 
    return Pred->getLocationContext();
  }

  const Stmt *getCallExpr() const { return CE; }

  const StackFrameContext *getCalleeContext() const { return CalleeCtx; }

  const CFGBlock *getBlock() const { return Block; }

  unsigned getIndex() const { return Index; }

  void generateNode(const ProgramState *state);
};

class CallExitNodeBuilder {
  CoreEngine &Eng;
  const ExplodedNode *Pred;

public:
  CallExitNodeBuilder(CoreEngine &eng, const ExplodedNode *pred)
    : Eng(eng), Pred(pred) {}

  const ExplodedNode *getPredecessor() const { return Pred; }

  const ProgramState *getState() const { return Pred->getState(); }

  void generateNode(const ProgramState *state);
}; 

} // end GR namespace

} // end clang namespace

#endif
