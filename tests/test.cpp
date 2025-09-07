#include "RefactorTool.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

class RefactorTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Инициализация будет в каждом тесте
  }

  Rewriter rewriter;
  std::unique_ptr<RefactorHandler> handler;
};

// Вспомогательная функция для получения AST из кода
std::unique_ptr<ASTUnit> getASTFromCode(const char *code) {
  return buildASTFromCode(code);
}

// Кастомный callback для тестирования методов
class MethodCallback : public MatchFinder::MatchCallback {
public:
  CXXMethodDecl *method = nullptr;

  void run(const MatchFinder::MatchResult &Result) override {
    method = const_cast<CXXMethodDecl *>(
        Result.Nodes.getNodeAs<CXXMethodDecl>("method"));
  }
};

// Тесты для невиртуальных деструкторов
TEST_F(RefactorTest, NonVirtualDtor_AddsVirtualKeyword) {
  const char *test_code = R"(
        class Base {
        public:
            ~Base() {} // Should become virtual
        };
    )";

  std::unique_ptr<ASTUnit> ast = getASTFromCode(test_code);
  ASTContext &context = ast->getASTContext();
  SourceManager &sm = context.getSourceManager();

  rewriter.setSourceMgr(sm, context.getLangOpts());
  handler = std::make_unique<RefactorHandler>(rewriter);

  auto matcher = cxxDestructorDecl(unless(isVirtual())).bind("nonVirtualDtor");
  MatchFinder finder;
  finder.addMatcher(matcher, handler.get());

  finder.matchAST(context);

  FileID main_file_id = sm.getMainFileID();
  std::string result = rewriter.getRewrittenText(
      SourceRange(sm.getLocForStartOfFile(main_file_id),
                  sm.getLocForEndOfFile(main_file_id)));

  EXPECT_TRUE(result.find("virtual ~Base") != std::string::npos);
}

TEST_F(RefactorTest, VirtualDtor_NoChanges) {
  const char *test_code = R"(
        class Base {
        public:
            virtual ~Base() {} // Already virtual, should not change
        };
    )";

  std::unique_ptr<ASTUnit> ast = getASTFromCode(test_code);
  ASTContext &context = ast->getASTContext();
  SourceManager &sm = context.getSourceManager();

  rewriter.setSourceMgr(sm, context.getLangOpts());
  handler = std::make_unique<RefactorHandler>(rewriter);

  auto matcher = cxxDestructorDecl().bind("nonVirtualDtor");
  MatchFinder finder;
  finder.addMatcher(matcher, handler.get());

  finder.matchAST(context);

  FileID main_file_id = sm.getMainFileID();
  std::string result = rewriter.getRewrittenText(
      SourceRange(sm.getLocForStartOfFile(main_file_id),
                  sm.getLocForEndOfFile(main_file_id)));

  EXPECT_TRUE(result.find("virtual ~Base") != std::string::npos);
}

// Тесты для методов без override - ПРЯМОЙ ТЕСТ ЛОГИКИ
TEST_F(RefactorTest, MethodWithoutOverride_AddsOverride) {
  const char *test_code = R"(
        class Base {
        public:
            virtual void foo() {}
        };
        
        class Derived : public Base {
        public:
            void foo() {} // Same signature as base
        };
    )";

  std::unique_ptr<ASTUnit> ast = getASTFromCode(test_code);
  ASTContext &context = ast->getASTContext();
  SourceManager &sm = context.getSourceManager();

  rewriter.setSourceMgr(sm, context.getLangOpts());
  handler = std::make_unique<RefactorHandler>(rewriter);

  // Находим метод вручную
  auto matcher = cxxMethodDecl(hasName("foo"),
                               hasParent(cxxRecordDecl(hasName("Derived"))))
                     .bind("method");

  MethodCallback callback;
  MatchFinder finder;
  finder.addMatcher(matcher, &callback);
  finder.matchAST(context);

  ASSERT_NE(callback.method, nullptr);

  // Проверяем, что метод не имеет атрибута override
  EXPECT_FALSE(callback.method->hasAttr<OverrideAttr>());

  // Проверяем, что метод переопределяет виртуальную функцию
  EXPECT_TRUE(callback.method->size_overridden_methods() > 0);

  // Проверяем, что метод не является чисто виртуальным
  EXPECT_FALSE(callback.method->isPureVirtual());
}

TEST_F(RefactorTest, MethodWithOverride_NoChanges) {
  const char *test_code = R"(
        class Base {
        public:
            virtual void foo() {}
        };
        
        class Derived : public Base {
        public:
            void foo() override {} // Already has override
        };
    )";

  std::unique_ptr<ASTUnit> ast = getASTFromCode(test_code);
  ASTContext &context = ast->getASTContext();
  SourceManager &sm = context.getSourceManager();

  rewriter.setSourceMgr(sm, context.getLangOpts());
  handler = std::make_unique<RefactorHandler>(rewriter);

  // Исправленный матчер
  auto matcher = cxxMethodDecl(hasName("foo"),
                               hasParent(cxxRecordDecl(hasName("Derived"))))
                     .bind("method");

  MethodCallback callback;
  MatchFinder finder;
  finder.addMatcher(matcher, &callback);
  finder.matchAST(context);

  ASSERT_NE(callback.method, nullptr);

  // Проверяем, что метод уже имеет атрибут override
  EXPECT_TRUE(callback.method->hasAttr<OverrideAttr>());
}

// Кастомный callback для тестирования range-for
class LoopVarCallback : public MatchFinder::MatchCallback {
public:
  VarDecl *loopVar = nullptr;

  void run(const MatchFinder::MatchResult &Result) override {
    loopVar = const_cast<VarDecl *>(Result.Nodes.getNodeAs<VarDecl>("loopVar"));
  }
};

// Тесты для range-for
TEST_F(RefactorTest, RangeForWithoutRef_AddsAmpersand) {
  const char *test_code = R"(
        #include <vector>
        void test() {
            std::vector<int> vec;
            for (const auto item : vec) {} // Should become const auto&
        }
    )";

  std::unique_ptr<ASTUnit> ast = getASTFromCode(test_code);
  ASTContext &context = ast->getASTContext();
  SourceManager &sm = context.getSourceManager();

  rewriter.setSourceMgr(sm, context.getLangOpts());
  handler = std::make_unique<RefactorHandler>(rewriter);

  auto matcher = varDecl(hasAncestor(cxxForRangeStmt())).bind("loopVar");

  LoopVarCallback callback;
  MatchFinder finder;
  finder.addMatcher(matcher, &callback);
  finder.matchAST(context);

  ASSERT_NE(callback.loopVar, nullptr);

  EXPECT_FALSE(callback.loopVar->getType()->isReferenceType());
  EXPECT_TRUE(callback.loopVar->getType().isConstQualified());
}

TEST_F(RefactorTest, RangeForWithRef_NoChanges) {
  const char *test_code = R"(
        #include <vector>
        void test() {
            std::vector<int> vec;
            for (const auto& item : vec) {} // Already has reference
        }
    )";

  std::unique_ptr<ASTUnit> ast = getASTFromCode(test_code);
  ASTContext &context = ast->getASTContext();
  SourceManager &sm = context.getSourceManager();

  rewriter.setSourceMgr(sm, context.getLangOpts());
  handler = std::make_unique<RefactorHandler>(rewriter);

  auto matcher = varDecl(hasAncestor(cxxForRangeStmt())).bind("loopVar");

  LoopVarCallback callback;
  MatchFinder finder;
  finder.addMatcher(matcher, &callback);
  finder.matchAST(context);

  ASSERT_NE(callback.loopVar, nullptr);

  EXPECT_TRUE(callback.loopVar->getType()->isReferenceType());
}