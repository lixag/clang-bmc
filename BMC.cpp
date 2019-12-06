#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/CFG.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include <vector>
// using namespace z3;
using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory BMCCategory("BMC options");

namespace {
class FunctionDeclVisitor : public RecursiveASTVisitor<FunctionDeclVisitor> {
    struct Formula {
        SmallVector<std::string, 10> Clauses;
    };
    using BlockPaths = std::vector<std::vector<CFGBlock *>>;
    using BlockPath = std::vector<CFGBlock *>;

    ASTContext *Ctx;
    BlockPaths Paths;
    unsigned long Depth = 7;

    void dfsHelper(BlockPaths &PS, BlockPath &P, CFGBlock *Src) {
        if (P.size() > Depth)
            return;
        if (!P.empty()) {
            if (auto *Label = P.back()->getLabel()) {
                if (auto *L = dyn_cast<LabelStmt>(Label)) {
                    if (strcmp(L->getName(), "Error") == 0) {
                        PS.push_back(P);
                        return;
                    }
                }
            }
        }
        P.push_back(Src);
        for (auto &E : Src->succs())
            dfsHelper(PS, P, E.getReachableBlock());
        P.pop_back();
    }

  public:
    FunctionDeclVisitor(ASTContext *ctx) : Ctx{ctx} {}

    void processStmt(Formula &F, const Stmt *stmt,
                     DenseMap<StringRef, int> &SSATable) {
        std::string formula;
        switch (stmt->getStmtClass()) {
        case Stmt::BinaryOperatorClass: {
            auto *B = cast<BinaryOperator>(stmt);
            auto *LHS = B->getLHS();
            auto *RHS = B->getRHS();

            // Normally it's assignment and comp here
            if (auto *Implicit = dyn_cast<ImplicitCastExpr>(LHS))
                LHS = Implicit->getSubExpr();
            if (auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
                if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                    std::string Var = VD->getQualifiedNameAsString();
                    if (B->isAssignmentOp()) {
                        formula.append(Var + std::to_string(++SSATable[Var]));
                        formula.append("==");
                    } else {
                        formula.append(Var + std::to_string(SSATable[Var]));
                        formula.append(B->getOpcodeStr());
                    }
                }
            }
            // else if (auto *UOP = dyn_cast<UnaryOperator>(LHS)) {
            //}

            if (auto *IntLit = dyn_cast<IntegerLiteral>(RHS)) {
                formula.append(IntLit->getValue().toString(10, false));
            } else if (auto *BOP = dyn_cast<BinaryOperator>(RHS)) {
                // should be add, sub, mul, div here
                auto *LHS1 = BOP->getLHS();
                auto *RHS1 = BOP->getRHS();
                if (auto *Implicit = dyn_cast<ImplicitCastExpr>(LHS1))
                    LHS1 = Implicit->getSubExpr();
                if (auto *Implicit = dyn_cast<ImplicitCastExpr>(RHS1))
                    RHS1 = Implicit->getSubExpr();
                if (auto *DRE = dyn_cast<DeclRefExpr>(LHS1)) {
                    if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                        std::string Var = VD->getQualifiedNameAsString();
                        formula.append(Var + std::to_string(SSATable[Var]));
                        formula.append(B->getOpcodeStr());
                    }
                }
                if (auto *IntLit1 = dyn_cast<IntegerLiteral>(RHS1)) {
                    formula.append(IntLit1->getValue().toString(10, false));
                } else {
                    if (auto *DRE = dyn_cast<DeclRefExpr>(RHS1)) {
                        if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                            std::string Var = VD->getQualifiedNameAsString();
                            formula.append(Var + std::to_string(SSATable[Var]));
                        }
                    }
                }
            }
            break;
        }
        case Stmt::DeclStmtClass: {
            auto *DST = cast<DeclStmt>(stmt);
            auto *SDC = DST->getSingleDecl();
            if (auto *VDC = dyn_cast<VarDecl>(SDC)) {
                std::string Var = VDC->getQualifiedNameAsString();
                formula.append(Var + std::to_string(SSATable[Var]));
                auto *Init = VDC->getInit();
                if (Init)
                    if (auto *IntLit = dyn_cast<IntegerLiteral>(Init))
                        formula.append(IntLit->getValue().toString(10, false));
            }
        }
        }
        F.Clauses.push_back(formula);
    }

    bool VisitFunctionDecl(FunctionDecl *Decl) {
        if (!Ctx->getSourceManager().isWrittenInMainFile(Decl->getBeginLoc()))
            return true;
        FullSourceLoc full_location = Ctx->getFullLoc(Decl->getBeginLoc());
        std::vector<Formula> Fs;
        DenseMap<StringRef, int> SSATable;
        if (full_location.isValid()) {
            if (Decl->hasBody()) {
                auto *FuncBody = Decl->getBody();
                std::unique_ptr<CFG> FuncCFG =
                    CFG::buildCFG(Decl, FuncBody, Ctx, CFG::BuildOptions());
                // FuncCFG->dump(LangOptions(), false);
                // FuncCFG->viewCFG(LangOptions());

                BlockPath Path;
                dfsHelper(Paths, Path, &FuncCFG->getEntry());
                errs() << "size: " << Paths.size() << "\n";
                for (auto &P : Paths) {
                    // std::error_code EC1;
                    // raw_fd_ostream OS1(std::to_string(cnt) + ".txt",
                    // EC1);
                    Formula F;
                    for (auto &N : P) {
                        for (auto &E : *N) {
                            auto *S = E.castAs<CFGStmt>().getStmt();
                            processStmt(F, S, SSATable);
                        }
                        // auto T = N->getTerminator();
                    }
                    Fs.push_back(F);
                }
            }
        }
        int cnt = 1;
        for (auto &F : Fs) {
            errs() << "path" << cnt << ": ";
            for (auto &Elem : F.Clauses)
                errs() << Elem << ' ';
            errs() << '\n';
            ++cnt;
        }

        errs() << "SSATable: ";
        for (auto &Elem : SSATable)
            errs() << Elem.first << ' ' << Elem.second << ' ';
        errs() << '\n';

        // transform Paths of length K to formulas
        // impl a parser

        //    int fileNum = 0;
        //    for (auto &F : Formulas) {
        //      std::error_code EC;
        //      raw_fd_ostream OS("Z3_code" + std::to_string(fileNum) +
        //      ".txt", EC); OS <<
        //      "#include<z3++>\n#include<iostream>\nusing namespace
        //      z3;\ncontext "
        //            "c;\nsolver s(c);\n";
        //
        //      for (auto &C : F.Consts)
        //        OS << "expr " << C << " = c.int_const(\"" << C << "\"\n";
        //      int cnt = 1;
        //      for (auto &Clause : F.Clauses) {
        //        OS << "expr conjecture" << cnt << " = " << Clause <<
        //        ";\n"; OS << "s.add(conjecture" << cnt << ");\n";
        //        ++cnt;
        //      }
        //
        //      OS << "s.check();\n";
        //      OS << R"("switch (s.check()) {\n
        //                     case unsat:
        //                        std::cout << "this path is valid\n";
        //                        break;
        //                     case sat:
        //                        std::cout <<"this path is not valid\n";
        //                        break;
        //                      case unknown:
        //                        std::cout <<"unknown\n";
        //                        break;
        //                      })";
        //      ++fileNum;
        //    }

        return true;
    }
}; // namespace

class FunctionDeclConsumer : public clang::ASTConsumer {
    FunctionDeclVisitor Visitor;

  public:
    explicit FunctionDeclConsumer(ASTContext *Ctx) : Visitor{Ctx} {}

    bool HandleTopLevelDecl(DeclGroupRef DG) override {
        for (auto D : DG)
            Visitor.TraverseDecl(D);
        return true;
    }
};

class FunctionDeclAction : public clang::ASTFrontendAction {
  public:
    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &Compiler,
                      llvm::StringRef InFile) override {
        return std::unique_ptr<clang::ASTConsumer>(
            new FunctionDeclConsumer{&Compiler.getASTContext()});
    }
};
} // namespace

int main(int argc, const char **argv) {
    CommonOptionsParser OptionsParser(argc, argv, BMCCategory);
    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<FunctionDeclAction>().get());
}
