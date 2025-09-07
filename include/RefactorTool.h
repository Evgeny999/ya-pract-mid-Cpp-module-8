#pragma once
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

class RefactorHandler : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  explicit RefactorHandler(clang::Rewriter &Rewrite) : Rewrite(Rewrite) {}

  virtual void
  run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

private:
  // 1. Невиртуальные деструкторы
  void handle_nv_dtor(const clang::CXXDestructorDecl *Dtor,
                      clang::SourceManager &SM, clang::ASTContext &Context);

  // 2. Методы без override
  void handle_miss_override(const clang::CXXMethodDecl *Method,
                            clang::SourceManager &SM,
                            clang::ASTContext &Context);

  // 3. range-for без &
  void handle_crange_for(const clang::VarDecl *LoopVar,
                         clang::SourceManager &SM, clang::ASTContext &Context);

private:
  clang::Rewriter &Rewrite;
  std::unordered_set<unsigned> processedLocations;
};

class ComplexConsumer : public clang::ASTConsumer {
public:
  explicit ComplexConsumer(clang::Rewriter &Rewrite);
  void HandleTranslationUnit(clang::ASTContext &Context) override;

private:
  RefactorHandler Handler;
  clang::ast_matchers::MatchFinder Finder;
};

class CodeRefactorAction : public clang::ASTFrontendAction {
public:
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI,
                    clang::StringRef file) override;
  virtual bool BeginSourceFileAction(clang::CompilerInstance &CI) override;
  virtual void EndSourceFileAction() override;

private:
  clang::Rewriter RewriterForCodeRefactor;
};
