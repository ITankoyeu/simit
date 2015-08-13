#include "lower_scatter_workspace.h"

#include <vector>
#include <set>
#include <map>
#include <string>

#include "loops.h"

#include "indexvar.h"
#include "ir.h"
#include "ir_visitor.h"
#include "ir_codegen.h"
#include "util.h"

using namespace std;

namespace simit {
namespace ir {

typedef vector<IndexVar> IndexTuple;
typedef map<IndexTuple, vector<const IndexedTensor *>> IndexTupleUses;
typedef map<IndexVar, vector<IndexVar>> IndexVarGraph;

ostream &operator<<(ostream &os, const IndexVarGraph &ivGraph) {
  os << "Index Variable Graph:"  << endl;
  for (auto &ij : ivGraph) {
    auto i = ij.first;
    for (auto &j : ij.second) {
      os << i << " -> " << j << endl;
    }
  }
  return os;
}

/// Build a map from index variable tuples to the IndexTensors they access:
/// - B+C   (i,j) -> B(i,j), C(i,j)
/// - B+C'  (i,j) -> B(i,j)
///         (j,i) -> C(j,i)
/// - B*C:  (i,k) -> B(i,k)
///         (k,j) -> C(k,j)
static IndexTupleUses getIndexTupleUses(const IndexExpr *indexExpr) {
  struct GetIndexTupleUsesVisitor : public IRVisitor {
    IndexTupleUses indexTupleUses;
    void visit(const IndexedTensor *indexedTensor) {
      indexTupleUses[indexedTensor->indexVars].push_back(indexedTensor);
    }
  };
  GetIndexTupleUsesVisitor visitor;
  indexExpr->accept(&visitor);
  return visitor.indexTupleUses;
}

/// Build a map from index variables to index variables they can reach through
/// a usage. This map encodes a directed index variable graph where vertices
/// are index variables, and where there exist an edge (i,j) if i and j are
/// ever used together to index a tensor that has an index from i to j. For now
/// we will assume we always have available all indices, but we may later want
/// to optimize for memory by computing a minimum set of indices we need.
/// - B+C: i -> j and j -> i
/// - B*C: i -> k and k -> i
///        k -> j and j -> k
static IndexVarGraph createIndexVarGraph(const IndexExpr *indexExpression) {
  IndexTupleUses indexTupleUses = getIndexTupleUses(indexExpression);

  IndexVarGraph indexVarGraph;
  for (auto &itu : indexTupleUses) {
    IndexTuple it = itu.first;
    for (size_t i=0; i < it.size() - 1; ++i) {
      for (size_t j=i+1; j < it.size(); ++j) {
        indexVarGraph[it[i]].push_back(it[j]);
        indexVarGraph[it[j]].push_back(it[i]);
      }
    }
  }
  return indexVarGraph;
}

static void createLoopNest(const IndexVarGraph &ivGraph,
                           const IndexVariableLoop &linkedLoop,
                           set<IndexVar> *visited,
                           vector<IndexVariableLoop> *loops) {
  for (const IndexVar &sink : ivGraph.at(linkedLoop.getIndexVar())) {
    if (!util::contains(*visited, sink)) {
      visited->insert(sink);
      loops->push_back(IndexVariableLoop(sink, linkedLoop));
      createLoopNest(ivGraph, sink, visited, loops);
    }
  }
}

/// Order the index variables into one loop per index variable, by traversing
/// the index variable graph
static
vector<IndexVariableLoop> createLoopNest(const IndexVarGraph &ivGraph,
                                         const vector<IndexVar> &sources){
  vector<IndexVariableLoop> loops;
  set<IndexVar> visited;
  for (auto &source : sources) {
    if (!util::contains(visited, source)) {
      visited.insert(source);
      IndexVariableLoop loop(source);
      loops.push_back(loop);
      createLoopNest(ivGraph, loop, &visited, &loops);
    }
  }
  return loops;
}

static vector<IndexVariableLoop> createLoopNest(const IndexExpr *indexExpr) {
  IndexVarGraph indexVariableGraph = createIndexVarGraph(indexExpr);
  return createLoopNest(indexVariableGraph, indexExpr->resultVars);
}

static Expr compareToNextIndexLocation(const TensorIndexVar &inductionVar) {
  return Lt::make(inductionVar.getCoordinateVar(),
                  inductionVar.loadCoordinate(1));
}

/// Create sparse while loop condition. Sparse while loops simultaneously
/// iterate over the coordinate variables of one or more tensors
static Expr subsetLoopCondition(const vector<TensorIndexVar> &inductionVars) {
  auto it = inductionVars.begin();
  auto end = inductionVars.end();
  Expr condition = compareToNextIndexLocation(*it++);
  for (; it != end; ++it) {
    condition = And::make(condition, compareToNextIndexLocation(*it));
  }
  return condition;
}

static Stmt createSubsetLoop(const Var &inductionVar,
                             const vector<TensorIndexVar> &tensorIndexVars,
                             Stmt body) {
  iassert(tensorIndexVars.size() > 0);

  // Create sparse while loop condition
  Expr condition = subsetLoopCondition(tensorIndexVars);

  // Initialize induction variables
  Stmt initInductionVar;
  if (tensorIndexVars.size() == 1) {
    initInductionVar = tensorIndexVars[0].initSinkVar(inductionVar);
  }
  else {
    vector<Expr> sinkInductionVars;
    vector<Stmt> initSinkInductionVars;
    for (const TensorIndexVar &tensorIndexVar : tensorIndexVars) {
      sinkInductionVars.push_back(tensorIndexVar.getSinkVar());
      initSinkInductionVars.push_back(tensorIndexVar.initSinkVar());
    }
    initInductionVar = Block::make(initSinkInductionVars);

    // Compute the loop induction variable as min of the tensor index variables
    initInductionVar = Block::make(initInductionVar,
                                   min(inductionVar, sinkInductionVars));
  }
  iassert(initInductionVar.defined());
  body = Block::make(initInductionVar, body);

  // Increment coordinate induction variables at the end of the loop body
  vector<Stmt> incCoordVarStmts;
  for (auto &inductionVar : tensorIndexVars) {
    incCoordVarStmts.push_back(increment(inductionVar.getCoordinateVar()));
  }
  body = Block::make(body, Block::make(incCoordVarStmts));

  // Create loop
  Stmt loop = While::make(condition, body);

  // Initialize coordinate induction variable
  vector<Stmt> initCoordVarsStmts;
  for (auto &inductionVar : tensorIndexVars) {
    initCoordVarsStmts.push_back(inductionVar.initCoordinateVar());
  }
  loop = Block::make(Block::make(initCoordVarsStmts), loop);

  return loop;
}

static Stmt createSubsetLoop(const Expr &target, const Var &inductionVar,
                             const SubsetLoop &subsetLoop) {
  Stmt computeStatement = Store::make(target, inductionVar,
                                      subsetLoop.getComputeExpression(),
                                      subsetLoop.getCompoundOperator());
  return createSubsetLoop(inductionVar, subsetLoop.getTensorIndexVars(),
                          computeStatement);
}

static string tensorSliceString(const vector<IndexVar> &vars,
                                const IndexVar &sliceVar) {
  unsigned sliceDimension = util::locate(vars, sliceVar);
  string result = "(";
  for (size_t i=0; i < vars.size(); ++i) {
    result += (i == sliceDimension) ? ":" : toString(vars[i]);
    if (i < vars.size()-1) {
      result += ",";
    }
  }
  result += ")";
  return result;
}

static string tensorSliceString(const Expr &expr, const IndexVar &sliceVar) {
  class SlicePrinter : public IRPrinter {
  public:
    SlicePrinter(const IndexVar &sliceVar) : IRPrinter(ss), sliceVar(sliceVar){}
    string toString(const Expr &expr) {
      skipTopExprParenthesis();
      print(expr);
      return ss.str();
    }
  private:
    stringstream ss;
    const IndexVar &sliceVar;
    void visit(const IndexedTensor *indexedTensor) {
      ss << indexedTensor->tensor
         << tensorSliceString(indexedTensor->indexVars, sliceVar);
    }
  };
  return SlicePrinter(sliceVar).toString(expr);;
}

Stmt lowerScatterWorkspace(Var target, const IndexExpr *indexExpression) {
  // Create loops
  vector<IndexVariableLoop> loops = createLoopNest(indexExpression);

  // Create workspace
  iassert(target.getType().isTensor());

  ScalarType workspaceComponentType =target.getType().toTensor()->componentType;
  IndexDomain workspaceDomain = loops[loops.size()-1].getIndexVar().getDomain();
  Type workspaceType = TensorType::make(workspaceComponentType,
                                        {workspaceDomain});
  Var workspace("workspace", workspaceType);

  // Emit loops
  Stmt loopNest;
  for (IndexVariableLoop &loop : util::reverse_iterator(loops)) {
    IndexVar indexVar = loop.getIndexVar();
    Var inductionVar  = loop.getInductionVar();

    // Dense loops
    if (!loop.isLinked()) {
      const IndexSet &indexSet = indexVar.getDomain().getIndexSets()[0];
      loopNest = For::make(inductionVar, indexSet, loopNest);
    }
    // Sparse/linked loops
    else {
      IndexVar linkedIndexVar = loop.getLinkedLoop().getIndexVar();
      Var linkedInductionVar  = loop.getLinkedLoop().getInductionVar();

      vector<SubsetLoop> subsetLoops = createSubsetLoops(indexExpression, loop);
//      std::cout << "Subset Loops:\n" << util::join(subsetLoops, "\n")<<"\n\n";

      // Create each subset loop and add their results to the workspace
      vector<Stmt> loopStatements;
      for (SubsetLoop &subsetLoop : subsetLoops) {
        Stmt loopStmt = createSubsetLoop(workspace, inductionVar, subsetLoop);
        string comment = "workspace " +
            util::toString(subsetLoop.getCompoundOperator())+"= " +
            tensorSliceString(subsetLoop.getIndexExpression(), indexVar);
        loopStatements.push_back(Comment::make(comment, loopStmt, "\n"));
      }
      iassert(loops.size() > 0);


      // Create the loop that copies the workspace to the target
      auto& resultVars = indexExpression->resultVars;

      TensorIndexVar resultIndexVar(inductionVar, linkedInductionVar, target,
                                      util::locate(resultVars,linkedIndexVar),
                                      util::locate(resultVars,indexVar));
      Stmt copyFromWorkspace = Store::make(target,
                                           resultIndexVar.getCoordinateVar(),
                                           Load::make(workspace, inductionVar));
      Stmt resetWorkspace = Store::make(workspace, inductionVar, 0.0);
      Stmt body = Block::make(copyFromWorkspace, resetWorkspace);
      Stmt loopStmt = createSubsetLoop(inductionVar, {resultIndexVar}, body);

      string comment = toString(target)
                     + tensorSliceString(resultVars, loop.getIndexVar())
                     + " = workspace";
      loopStatements.push_back(Comment::make(comment, loopStmt));

      loopNest = Block::make(loopStatements);
    }
  }

  stringstream comment;
  comment << util::toString(target)
          << "(" + util::join(indexExpression->resultVars, ",") << ") = ";
  IRPrinter printer(comment);
  printer.skipTopExprParenthesis();
  printer.print(indexExpression->value);
  return Comment::make(comment.str(), loopNest);
}

}}