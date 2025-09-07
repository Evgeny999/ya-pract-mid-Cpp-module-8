#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"
#include <iostream>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
  auto &SM = *Result.SourceManager;
  auto &Context = *Result.Context;

  // Проверяем, находится ли код в основном файле
  auto isInMainFile = [&SM](SourceLocation Loc) {
    return SM.isInMainFile(
        SM.getFileLoc(Loc)); // Используем getFileLoc для нормализации
  };

  if (const auto *Dtor =
          Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor")) {
    // Проверяем местоположение класса, а не деструктора
    if (const CXXRecordDecl *Class = Dtor->getParent()) {
      if (isInMainFile(Class->getLocation())) {
        std::cout << "Found destructor in main file\n";
        handle_nv_dtor(Dtor, SM, Context);
      }
    }
  }

  if (const auto *Method =
          Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride")) {
    // Проверяем местоположение класса, а не метода
    if (const CXXRecordDecl *Class = Method->getParent()) {
      if (isInMainFile(Class->getLocation())) {
        std::cout << "Found method in main file\n";
        handle_miss_override(Method, SM, Context);
      }
    }
  }

  if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
    if (isInMainFile(LoopVar->getLocation())) {
      std::cout << "Found loop variable in main file\n";
      handle_crange_for(LoopVar, SM, Context);
    } else {
      std::cout << "Loop variable not in main file\n";
    }
  }
}

// Обработка невиртуальных деструкторов

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor,
                                     SourceManager &SM, ASTContext &Context) {
  SourceLocation tildeLoc = Dtor->getLocation();

  // Проверяем, что это действительно символ ~
  const char *tildePtr = SM.getCharacterData(tildeLoc);
  if (*tildePtr != '~') {
    // Пропускаем - это не настоящий деструктор
    std::cout << "Skipping - not a tilde character: '" << *tildePtr << "'\n";
    return;
  }

  unsigned locationHash = tildeLoc.getRawEncoding();
  if (processedLocations.find(locationHash) != processedLocations.end()) {
    return;
  }
  processedLocations.insert(locationHash);

  if (Dtor->isVirtual()) {
    return;
  }

  // Ищем "virtual" в объявлении
  SourceLocation searchStart = SM.getLocForStartOfFile(SM.getFileID(tildeLoc));
  StringRef textBefore =
      Lexer::getSourceText(CharSourceRange::getCharRange(searchStart, tildeLoc),
                           SM, Context.getLangOpts());

  // Более точная проверка на "virtual" как отдельное слово
  std::string word = "virtual";
  // size_t virtualPos = textBefore.rfind("virtual");
  size_t virtualPos = textBefore.rfind(word);
  if (virtualPos != StringRef::npos) {
    // Проверяем, что это отдельное слово
    bool isStartOk = (virtualPos == 0) || !isalnum(textBefore[virtualPos - 1]);
    bool isEndOk = (virtualPos + word.size() >= textBefore.size()) ||
                   !isalnum(textBefore[virtualPos + word.size()]);

    if (isStartOk && isEndOk) {
      return; // Уже есть virtual
    }
  }

  // Вставляем "virtual " перед символом ~
  Rewrite.InsertTextBefore(tildeLoc, "virtual ");
}

// Обработка методов без override
void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method,
                                           SourceManager &SM,
                                           ASTContext &Context) {
  unsigned locationHash = Method->getLocation().getRawEncoding();
  if (processedLocations.find(locationHash) != processedLocations.end()) {
    return;
  }
  processedLocations.insert(locationHash);

  // Пропускаем деструкторы (они обрабатываются отдельно)
  if (isa<CXXDestructorDecl>(Method)) {

    return;
  }

  // Находим позицию для вставки override
  SourceLocation insertLoc = Method->getEndLoc();

  // Для функций с телом ищем закрывающую скобку
  if (Method->hasBody()) {
    if (const Stmt *body = Method->getBody()) {
      insertLoc = body->getBeginLoc();
    }
  }

  // Проверяем, нет ли уже override
  if (Method->hasAttr<OverrideAttr>()) {
    return;
  }

  // Вставляем " override" перед телом функции или в конце
  Rewrite.InsertTextBefore(insertLoc, " override");
}

// Обработка range-for без ссылки
void RefactorHandler::handle_crange_for(const VarDecl *LoopVar,
                                        SourceManager &SM,
                                        ASTContext &Context) {
  unsigned locationHash = LoopVar->getLocation().getRawEncoding();
  if (processedLocations.find(locationHash) != processedLocations.end()) {
    return;
  }
  processedLocations.insert(locationHash);

  QualType type = LoopVar->getType();
  if (type->isReferenceType() || type->isBuiltinType()) {
    return;
  }

  if (!type.isConstQualified()) {
    return;
  }

  // Вставляем & перед именем переменной, а не после типа
  SourceLocation varNameLoc = LoopVar->getLocation();

  // Проверяем, нет ли уже амперсанда перед именем переменной
  SourceLocation checkStart = varNameLoc.getLocWithOffset(-10);
  if (checkStart.isInvalid()) {
    checkStart = SM.getLocForStartOfFile(SM.getFileID(varNameLoc));
  }

  StringRef textBeforeName = Lexer::getSourceText(
      CharSourceRange::getCharRange(checkStart, varNameLoc), SM,
      Context.getLangOpts());

  if (textBeforeName.contains("&")) {
    return;
  }

  // Вставляем "&" перед именем переменной
  Rewrite.InsertTextBefore(varNameLoc, "&");
}

// Матчер для невиртуальных деструкторов
auto NvDtorMatcher() {
  return cxxDestructorDecl(unless(isVirtual()), // Не виртуальный
                           ofClass(cxxRecordDecl(hasDescendant(
                               cxxRecordDecl()))), // Любой потомок
                           unless(hasParent(cxxRecordDecl(isImplicit()))))
      .bind("nonVirtualDtor");
}

// Матчер для методов без override
auto NoOverrideMatcher() {
  return cxxMethodDecl(
             // Метод переопределяет виртуальную функцию из базового класса
             isOverride(), // Это проверяет, переопределяет ли метод виртуальную
                           // функцию
             unless(hasAttr(attr::Override)), // Но не имеет атрибута override
             unless(isPure()),                // Не чисто виртуальный
             unless(isImplicit()),            // Не неявный
             unless(hasParent(cxxRecordDecl(isImplicit()))))
      .bind("missingOverride");
}

// Матчер для range-for без ссылки
auto NoRefConstVarInRangeLoopMatcher() {
  std::cout << "NoRefConstVarInRangeLoopMatcher called\n";
  return varDecl(hasAncestor(cxxForRangeStmt()), unless(isImplicit()))
      .bind("loopVar");
}

ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
  Finder.addMatcher(NvDtorMatcher(), &Handler);
  Finder.addMatcher(NoOverrideMatcher(), &Handler);
  Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) {
  Finder.matchAST(Context);
}

std::unique_ptr<ASTConsumer>
CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
  RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
  RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return true;
}

void CodeRefactorAction::EndSourceFileAction() {
  if (RewriterForCodeRefactor.overwriteChangedFiles()) {
    llvm::errs() << "Error applying changes to files.\n";
  }
}

/*int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}*/