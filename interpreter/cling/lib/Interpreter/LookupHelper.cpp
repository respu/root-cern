//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// version: $Id: AST.cpp 45014 2012-07-11 20:31:42Z vvassilev $
// author:  Vassil Vassilev <vvasilev@cern.ch>
//------------------------------------------------------------------------------

#include "cling/Interpreter/LookupHelper.h"

#include "clang/AST/ASTContext.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"

using namespace clang;

namespace cling {

  ///\brief Helper class that allows us to call protected functions in the 
  /// Parser. This is especially useful for the string-based lookup.
  /// 
  class ParserExt : public Parser {
  public:
    static TypeResult ParseTypeNameFwd(Parser* P, SourceRange *Range = 0,
                                       Declarator::TheContext Context
                                       = Declarator::TypeNameContext,
                                       AccessSpecifier AS = AS_none,
                                       Decl **OwnedType = 0) {
      return ((ParserExt*)P)->ParseTypeName(Range, Context, AS, OwnedType);
    }

    static const Token& NextTokenFwd(Parser* P) {
      return ((ParserExt*)P)->NextToken();
    }

    static bool SkipUntilFwd(Parser* P, tok::TokenKind T, bool StopAtSemi = true,
                             bool DontConsume = false, 
                             bool StopAtCodeCompletion = false) {
      
      return ((ParserExt*)P)->SkipUntil(T, StopAtSemi, DontConsume, 
                                        StopAtCodeCompletion);
    }

    static bool TryAnnotateCXXScopeTokenFwd(Parser* P, 
                                         bool EnteringContext = false) {

      return ((ParserExt*)P)->TryAnnotateCXXScopeToken(EnteringContext);
    }
    static  bool TryAnnotateTypeOrScopeTokenFwd(Parser* P, 
                                             bool EnteringContext = false,
                                             bool NeedType = false) {

      return ((ParserExt*)P)->TryAnnotateTypeOrScopeToken(EnteringContext);
    }

    static ParsedType getTypeAnnotationFwd(Token &Tok) {
      return ParserExt::getTypeAnnotation(Tok);
    }

    static SourceLocation ConsumeTokenFwd(Parser* P) {
      return ((ParserExt*)P)->ConsumeToken();
    }

    static bool ParseUnqualifiedIdFwd(Parser* P,
                                      CXXScopeSpec &SS, bool EnteringContext,
                                      bool AllowDestructorName,
                                      bool AllowConstructorName,
                                      ParsedType ObjectType,
                                      SourceLocation& TemplateKWLoc,
                                      UnqualifiedId &Result) {

      return ((ParserExt*)P)->ParseUnqualifiedId(SS, EnteringContext, 
                                                 AllowDestructorName,
                                                 AllowConstructorName,
                                                 ObjectType,
                                                 TemplateKWLoc,
                                                 Result);
    }

    static ExprResult ParseAssignmentExpressionFwd(Parser* P, 
                                                   TypeCastState isTypeCast 
                                                   = NotTypeCast) {
      return ((ParserExt*)P)->ParseAssignmentExpression(isTypeCast);

    }


    static void EnterScopeFwd(Parser* P, unsigned ScopeFlags) {
      ((ParserExt*)P)->EnterScope(ScopeFlags);
    }

    static void ExitScopeFwd(Parser* P) {
      ((ParserExt*)P)->ExitScope();
    }
  };

  ///\brief Cleanup Parser state after a failed lookup.
  /// 
  /// After a failed lookup we need to discard the remaining unparsed input,
  /// restore the original state of the incremental parsing flag, signal
  /// the diagnostic client that the current input file is done, clear any
  /// pending diagnostics, restore the suppress diagnostics flag, and restore
  /// the spell checking language options.
  ///
  class ParserStateRAII {
  private:
    Sema& S;
    Parser* P;
    Preprocessor& PP;
    DiagnosticConsumer* DClient;
    bool ResetIncrementalProcessing;
    bool OldSuppressAllDiagnostics;
    bool OldSpellChecking;

  public:
    ParserStateRAII(Sema& s, Parser* p, bool rip, bool sad, bool sc)
       : S(s), P(p), PP(s.getPreprocessor()), 
         DClient(s.getDiagnostics().getClient()), 
         ResetIncrementalProcessing(rip),
         OldSuppressAllDiagnostics(sad), OldSpellChecking(sc)
    {}

    ~ParserStateRAII()
    {
      //
      // Advance the parser to the end of the file, and pop the include stack.
      //
      // Note: Consuming the EOF token will pop the include stack.
      //
      ParserExt::SkipUntilFwd(P, tok::eof, /*StopAtSemi*/false, 
                              /*DontConsume*/false, 
                              /*StopAtCodeCompletion*/false);
      if (ResetIncrementalProcessing) {
        PP.enableIncrementalProcessing(false);
      }
      DClient->EndSourceFile();
      S.getDiagnostics().Reset();
      PP.getDiagnostics().setSuppressAllDiagnostics(OldSuppressAllDiagnostics);
      const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking =
         OldSpellChecking;
    }
  };

  QualType LookupHelper::findType(llvm::StringRef typeName) const {
    //
    //  Our return value.
    //
    QualType TheQT;

    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = P.getPreprocessor();
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    bool OldSuppressAllDiagnostics =
      PP.getDiagnostics().getSuppressAllDiagnostics();
    PP.getDiagnostics().setSuppressAllDiagnostics(true);
    //
    //  Tell the parser to not attempt spelling correction.
    //
    bool OldSpellChecking = PP.getLangOpts().SpellChecking;
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = 0;
    //
    //  Tell the diagnostic consumer we are switching files.
    //
    DiagnosticConsumer* DClient = S.getDiagnostics().getClient();
    DClient->BeginSourceFile(PP.getLangOpts(), &PP);
    //
    //  Create a fake file to parse the type name.
    //
    llvm::MemoryBuffer* SB = llvm::MemoryBuffer::getMemBufferCopy(
      std::string(typeName) + "\n", "lookup.type.by.name.file");
    FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    bool ResetIncrementalProcessing = false;
    if (!PP.isIncrementalProcessingEnabled()) {
      ResetIncrementalProcessing = true;
      PP.enableIncrementalProcessing();
    }
    //
    //  Switch to the new file the way #include does.
    //
    //  Note: To switch back to the main file we must consume an eof token.
    //
    PP.EnterSourceFile(FID, /*DirLookup=*/0, SourceLocation());
    PP.Lex(const_cast<Token&>(P.getCurToken()));
    //
    //  Try parsing the type name.
    //
    TypeResult Res(ParserExt::ParseTypeNameFwd(&P));
    if (Res.isUsable()) {
      // Accept it only if the whole name was parsed.
      if (ParserExt::NextTokenFwd(&P).getKind() == clang::tok::eof) {
        TypeSourceInfo* TSI = 0;
        // The QualType returned by the parser is an odd QualType
        // (type + TypeSourceInfo) and cannot be used directly.
        TheQT = clang::Sema::GetTypeFromParser(Res.get(), &TSI);
      }
    }
    //
    // Advance the parser to the end of the file, and pop the include stack.
    //
    // Note: Consuming the EOF token will pop the include stack.
    //
    ParserExt::SkipUntilFwd(&P, tok::eof, /*StopAtSemi*/false, 
                            /*DontConsume*/false,
                            /*StopAtCodeCompletion*/false);
    if (ResetIncrementalProcessing) {
      PP.enableIncrementalProcessing(false);
    }
    DClient->EndSourceFile();
    S.getDiagnostics().Reset();
    S.getDiagnostics().setSuppressAllDiagnostics(OldSuppressAllDiagnostics);
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = OldSpellChecking;
    return TheQT;
  }

  const Decl* LookupHelper::findScope(llvm::StringRef className,
                                      const Type** resultType /* = 0 */) const {
    //
    //  Our return value.
    //
    const Type* TheType = 0;
    const Type** setResultType = &TheType;
    if (resultType)
      setResultType = resultType;
    *setResultType = 0;

    const Decl* TheDecl = 0;
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = P.getPreprocessor();
    ASTContext& Context = S.getASTContext();
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    bool OldSuppressAllDiagnostics =
      PP.getDiagnostics().getSuppressAllDiagnostics();
    PP.getDiagnostics().setSuppressAllDiagnostics(true);
    //
    //  Tell the parser to not attempt spelling correction.
    //
    bool OldSpellChecking = PP.getLangOpts().SpellChecking;
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = 0;
    //
    //  Tell the diagnostic consumer we are switching files.
    //
    DiagnosticConsumer* DClient = S.getDiagnostics().getClient();
    DClient->BeginSourceFile(S.getLangOpts(), &PP);
    //
    //  Convert the class name to a nested name specifier for parsing.
    //
    std::string classNameAsNNS = className.str() + "::\n";
    //
    //  Create a fake file to parse the class name.
    //
    llvm::MemoryBuffer* SB = llvm::MemoryBuffer::getMemBufferCopy(
      classNameAsNNS, "lookup.class.by.name.file");
    FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    bool ResetIncrementalProcessing = false;
    if (!PP.isIncrementalProcessingEnabled()) {
      ResetIncrementalProcessing = true;
      PP.enableIncrementalProcessing();
    }
    //
    //  Switch to the new file the way #include does.
    //
    //  Note: To switch back to the main file we must consume an eof token.
    //
    PP.EnterSourceFile(FID, 0, SourceLocation());
    PP.Lex(const_cast<Token&>(P.getCurToken()));
    //
    //  Setup to reset parser state on exit.
    //
    ParserStateRAII ResetParserState(S, &P, ResetIncrementalProcessing,
                                     OldSuppressAllDiagnostics, 
                                     OldSpellChecking);
    //
    //  Prevent failing on an assert in TryAnnotateCXXScopeToken.
    //
    if (!P.getCurToken().is(clang::tok::identifier) && !P.getCurToken().
          is(clang::tok::coloncolon) && !(P.getCurToken().is(
          clang::tok::annot_template_id) && ParserExt::NextTokenFwd(&P).is(
          clang::tok::coloncolon)) && !P.getCurToken().is(
          clang::tok::kw_decltype)) {
      // error path
      return TheDecl;
    }
    //
    //  Try parsing the name as a nested-name-specifier.
    //
    if (ParserExt::TryAnnotateCXXScopeTokenFwd(&P, false)) {
      // error path
      return TheDecl;
    }
    if (P.getCurToken().getKind() == tok::annot_cxxscope) {
      CXXScopeSpec SS;
      S.RestoreNestedNameSpecifierAnnotation(P.getCurToken().getAnnotationValue(),
                                             P.getCurToken().getAnnotationRange(),
                                             SS);
      if (SS.isValid()) {
        NestedNameSpecifier* NNS = SS.getScopeRep();
        NestedNameSpecifier::SpecifierKind Kind = NNS->getKind();
        // Only accept the parse if we consumed all of the name.
        if (ParserExt::NextTokenFwd(&P).getKind() == clang::tok::eof) {
          //
          //  Be careful, not all nested name specifiers refer to classes
          //  and namespaces, and those are the only things we want.
          //
          switch (Kind) {
            case NestedNameSpecifier::Identifier: {
                // Dependent type.
                // We do not accept these.
              }
              break;
            case NestedNameSpecifier::Namespace: {
                // Namespace.
                NamespaceDecl* NSD = NNS->getAsNamespace();
                NSD = NSD->getCanonicalDecl();
                TheDecl = NSD;
              }
              break;
            case NestedNameSpecifier::NamespaceAlias: {
                // Namespace alias.
                // Note: In the future, should we return the alias instead?
                NamespaceAliasDecl* NSAD = NNS->getAsNamespaceAlias();
                NamespaceDecl* NSD = NSAD->getNamespace();
                NSD = NSD->getCanonicalDecl();
                TheDecl = NSD;
              }
              break;
            case NestedNameSpecifier::TypeSpec:
                // Type name.
            case NestedNameSpecifier::TypeSpecWithTemplate: {
                // Type name qualified with "template".
                // Note: Do we need to check for a dependent type here?
                NestedNameSpecifier *prefix = NNS->getPrefix();
                if (prefix) {
                   QualType temp = Context.getElaboratedType(ETK_None,prefix,QualType(NNS->getAsType(),0));
                   *setResultType = temp.getTypePtr();
                } else {
                   *setResultType = NNS->getAsType();
                }
                const TagType* TagTy = (*setResultType)->getAs<TagType>();
                if (TagTy) {
                  // It is a class, struct, or union.
                  TagDecl* TD = TagTy->getDecl();
                  if (TD) {
                    // Make sure it is not just forward declared, and
                    // instantiate any templates.
                    if (!S.RequireCompleteDeclContext(SS, TD)) {
                      // Success, type is complete, instantiations have
                      // been done.
                      TagDecl* Def = TD->getDefinition();
                      if (Def) {
                        TheDecl = Def;
                      }
                    }
                  }
                }
              }
              break;
            case clang::NestedNameSpecifier::Global: {
                // Name was just "::" and nothing more.
                TheDecl = Context.getTranslationUnitDecl();
              }
              break;
          }
          return TheDecl;
        }
      }
    }
    //
    //  Cleanup after failed parse as a nested-name-specifier.
    //
    ParserExt::SkipUntilFwd(&P,clang::tok::eof, /*StopAtSemi*/false, 
                            /*DontConsume*/false, /*StopAtCodeCompletion*/false);
    DClient->EndSourceFile();
    S.getDiagnostics().Reset();
    //
    //  Setup to reparse as a type.
    //
    DClient->BeginSourceFile(PP.getLangOpts(), &PP);
    {
      llvm::MemoryBuffer* SB =
        llvm::MemoryBuffer::getMemBufferCopy(className.str() + "\n",
          "lookup.type.file");
      clang::FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
      PP.EnterSourceFile(FID, 0, clang::SourceLocation());
      PP.Lex(const_cast<clang::Token&>(P.getCurToken()));
    }
    //
    //  Now try to parse the name as a type.
    //
    if (ParserExt::TryAnnotateTypeOrScopeTokenFwd(&P, false, false)) {
      // error path
      return TheDecl;
    }
    if (P.getCurToken().getKind() == tok::annot_typename) {
      ParsedType T = ParserExt::getTypeAnnotationFwd(
        const_cast<Token&>(P.getCurToken()));
      // Only accept the parse if we consumed all of the name.
      if (ParserExt::NextTokenFwd(&P).getKind() == clang::tok::eof) {
        QualType QT = T.get();
        if (const EnumType* ET = QT->getAs<EnumType>()) {
           EnumDecl* ED = ET->getDecl();
           TheDecl = ED->getDefinition();
           *setResultType = QT.getTypePtr();
        }
      }
    }
    return TheDecl;
  }

  static bool FuncArgTypesMatch(const ASTContext& C, 
                             const llvm::SmallVector<QualType, 4>& GivenArgTypes,
                                const FunctionProtoType* FPT) {
    // FIXME: What if FTP->arg_size() != GivenArgTypes.size()?
    FunctionProtoType::arg_type_iterator ATI = FPT->arg_type_begin();
    FunctionProtoType::arg_type_iterator E = FPT->arg_type_end();
    llvm::SmallVector<QualType, 4>::const_iterator GAI = GivenArgTypes.begin();
    for (; ATI && (ATI != E); ++ATI, ++GAI) {
      if (!C.hasSameType(*ATI, *GAI)) {
        return false;
      }
    }
    return true;
  }

  static bool IsOverload(const ASTContext& C,
                         const TemplateArgumentListInfo* FuncTemplateArgs,
                         const llvm::SmallVector<QualType, 4>& GivenArgTypes, 
                         FunctionDecl* FD, bool UseUsingDeclRules) {
    //FunctionTemplateDecl* FTD = FD->getDescribedFunctionTemplate();
    QualType FQT = C.getCanonicalType(FD->getType());
    if (llvm::isa<FunctionNoProtoType>(FQT.getTypePtr())) {
      // A K&R-style function (no prototype), is considered to match the args.
      return false;
    }
    const FunctionProtoType* FPT = llvm::cast<FunctionProtoType>(FQT);
    if ((GivenArgTypes.size() != FPT->getNumArgs()) ||
        //(GivenArgsAreEllipsis != FPT->isVariadic()) ||
        !FuncArgTypesMatch(C, GivenArgTypes, FPT)) {
      return true;
    }
    return false;
  }

  const FunctionDecl* LookupHelper::findFunctionProto(const Decl* scopeDecl,
                                                      llvm::StringRef funcName, 
                                               llvm::StringRef funcProto) const {
    //
    //  Our return value.
    //
    FunctionDecl* TheDecl = 0;
    //
    //  Some utilities.
    //
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = S.getPreprocessor();
    ASTContext& Context = S.getASTContext();
    //
    //  Get the DeclContext we will search for the function.
    //
    NestedNameSpecifier* classNNS = 0;
    if (const NamespaceDecl* NSD = dyn_cast<NamespaceDecl>(scopeDecl)) {
      classNNS = NestedNameSpecifier::Create(Context, 0,
                                             const_cast<NamespaceDecl*>(NSD));
    }
    else if (const RecordDecl* RD = dyn_cast<RecordDecl>(scopeDecl)) {
      const Type* T = Context.getRecordType(RD).getTypePtr();
      classNNS = NestedNameSpecifier::Create(Context, 0, false, T);
    }
    else if (llvm::isa<TranslationUnitDecl>(scopeDecl)) {
      classNNS = NestedNameSpecifier::GlobalSpecifier(Context);
    }
    else {
      // Not a namespace or class, we cannot use it.
      return 0;
    }
    DeclContext* foundDC = dyn_cast<DeclContext>(const_cast<Decl*>(scopeDecl));
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    bool OldSuppressAllDiagnostics =
      PP.getDiagnostics().getSuppressAllDiagnostics();
    PP.getDiagnostics().setSuppressAllDiagnostics(true);
    //
    //  Tell the parser to not attempt spelling correction.
    //
    bool OldSpellChecking = PP.getLangOpts().SpellChecking;
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = 0;
    //
    //  Tell the diagnostic consumer we are switching files.
    //
    DiagnosticConsumer* DClient = S.getDiagnostics().getClient();
    DClient->BeginSourceFile(PP.getLangOpts(), &PP);
    //
    //  Create a fake file to parse the prototype.
    //
    llvm::MemoryBuffer* SB 
      = llvm::MemoryBuffer::getMemBufferCopy(funcProto.str() 
                                             + "\n", "func.prototype.file");
    FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    bool ResetIncrementalProcessing = false;
    if (!PP.isIncrementalProcessingEnabled()) {
      ResetIncrementalProcessing = true;
      PP.enableIncrementalProcessing();
    }
    //
    //  Switch to the new file the way #include does.
    //
    //  Note: To switch back to the main file we must consume an eof token.
    //
    PP.EnterSourceFile(FID, /*DirLookup=*/0, SourceLocation());
    PP.Lex(const_cast<Token&>(P.getCurToken()));
    //
    //  Setup to reset parser state on exit.
    //
    ParserStateRAII ResetParserState(S, &P, ResetIncrementalProcessing,
                                     OldSuppressAllDiagnostics, 
                                     OldSpellChecking);
    //
    //  Parse the prototype now.
    //
    llvm::SmallVector<QualType, 4> GivenArgTypes;
    llvm::SmallVector<Expr*, 4> GivenArgs;
    while (P.getCurToken().isNot(tok::eof)) {
      TypeResult Res(ParserExt::ParseTypeNameFwd(&P));
      if (!Res.isUsable()) {
        // Bad parse, done.
        return TheDecl;
      }
      TypeSourceInfo *TSI = 0;
      // The QualType returned by the parser is an odd QualType (type + TypeSourceInfo)
      // and can not be used directly.
      clang::QualType QT(clang::Sema::GetTypeFromParser(Res.get(),&TSI));
      QT = QT.getCanonicalType();
      GivenArgTypes.push_back(QT);
      {
        // FIXME: Make an attempt to release these.
        clang::QualType NonRefQT(QT.getNonReferenceType());
        Expr* val = new (Context) OpaqueValueExpr(SourceLocation(), NonRefQT,
          Expr::getValueKindForType(NonRefQT));
        GivenArgs.push_back(val);
      }
      // Type names should be comma separated.
      if (!P.getCurToken().is(clang::tok::comma)) {
        break;
      }
      // Eat the comma.
      ParserExt::ConsumeTokenFwd(&P);
    }
    if (P.getCurToken().isNot(tok::eof)) {
      // We did not consume all of the prototype, bad parse.
      return TheDecl;
    }
    //
    //  Cleanup after prototype parse.
    //
    ParserExt::SkipUntilFwd(&P, clang::tok::eof, /*StopAtSemi*/false, 
                            /*DontConsume*/false, /*StopAtCodeCompletion*/false);
    DClient->EndSourceFile();
    S.getDiagnostics().Reset();
    //
    //  Create a fake file to parse the function name.
    //
    {
      llvm::MemoryBuffer* SB 
        = llvm::MemoryBuffer::getMemBufferCopy(funcName.str()
                                               + "\n", "lookup.funcname.file");
      clang::FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
      PP.EnterSourceFile(FID, /*DirLookup=*/0, clang::SourceLocation());
      PP.Lex(const_cast<clang::Token&>(P.getCurToken()));
    }
    {
      //
      //  Parse the function name.
      //
      SourceLocation TemplateKWLoc;
      UnqualifiedId FuncId;
      CXXScopeSpec SS;
      SS.MakeTrivial(Context, classNNS, SourceRange());
      //
      //  Make the class we are looking up the function
      //  in the current scope to please the constructor
      //  name lookup.  We do not need to do this otherwise,
      //  and may be able to remove it in the future if
      //  the way constructors are looked up changes.
      //
      ParserExt::EnterScopeFwd(&P, Scope::DeclScope);
      S.EnterDeclaratorContext(P.getCurScope(), foundDC);
      if (ParserExt::ParseUnqualifiedIdFwd(&P, SS, /*EnteringContext*/false,
                                           /*AllowDestructorName*/true,
                                           /*AllowConstructorName*/true,
                                           clang::ParsedType(), TemplateKWLoc,
                                           FuncId)) {
        // Bad parse.
        // Destroy the scope we created first, and
        // restore the original.
        S.ExitDeclaratorContext(P.getCurScope());
        ParserExt::ExitScopeFwd(&P);

        // Then cleanup and exit.
        return TheDecl;
      }
      //
      //  Get any template args in the function name.
      //
      TemplateArgumentListInfo FuncTemplateArgsBuffer;
      DeclarationNameInfo FuncNameInfo;
      const TemplateArgumentListInfo* FuncTemplateArgs;
      S.DecomposeUnqualifiedId(FuncId, FuncTemplateArgsBuffer, FuncNameInfo, 
                               FuncTemplateArgs);
      //
      //  Lookup the function name in the given class now.
      //
      DeclarationName FuncName = FuncNameInfo.getName();
      SourceLocation FuncNameLoc = FuncNameInfo.getLoc();
      LookupResult Result(S, FuncName, FuncNameLoc, Sema::LookupMemberName, 
                          Sema::NotForRedeclaration);
      if (!S.LookupQualifiedName(Result, foundDC)) {
        // Lookup failed.
        // Destroy the scope we created first, and
        // restore the original.
        S.ExitDeclaratorContext(P.getCurScope());
        ParserExt::ExitScopeFwd(&P);
        // Then cleanup and exit.
        return TheDecl;
      }
      // Destroy the scope we created, and
      // restore the original.
      S.ExitDeclaratorContext(P.getCurScope());
      ParserExt::ExitScopeFwd(&P);
      //
      //  Check for lookup failure.
      //
      if (!(Result.getResultKind() == LookupResult::Found) &&
          !(Result.getResultKind() == LookupResult::FoundOverloaded)) {
        // Lookup failed.
        return TheDecl;
      }
      //
      //  Now that we have a set of matching function names
      //  in the class, we have to choose the one being asked
      //  for given the passed template args and prototype.
      //
      for (LookupResult::iterator I = Result.begin(), E = Result.end();
          I != E; ++I) {
        NamedDecl* ND = *I;
        //
        //  Check if this decl is from a using decl, it will not
        //  be a match in some cases.
        //
        bool IsUsingDecl = false;
        if (llvm::isa<UsingShadowDecl>(ND)) {
          IsUsingDecl = true;
          ND = llvm::cast<UsingShadowDecl>(ND)->getTargetDecl();
        }
        //
        //  If found declaration was introduced by a using declaration,
        //  we'll need to use slightly different rules for matching.
        //  Essentially, these rules are the normal rules, except that
        //  function templates hide function templates with different
        //  return types or template parameter lists.
        //
        bool UseMemberUsingDeclRules = IsUsingDecl && foundDC->isRecord();
        if (FunctionTemplateDecl* FTD = dyn_cast<FunctionTemplateDecl>(ND)) {
          // This decl is a function template.
          //
          //  Do template argument deduction and function argument matching.
          //
          FunctionDecl* Specialization;
          sema::TemplateDeductionInfo TDI(Context, SourceLocation());
          Sema::TemplateDeductionResult TDR 
            = S.DeduceTemplateArguments(FTD,
                         const_cast<TemplateArgumentListInfo*>(FuncTemplateArgs),
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                        Specialization, TDI);
          if (TDR == Sema::TDK_Success) {
            // We have a template argument match and func arg match.
            TheDecl = Specialization;
            break;
          }
        } else if (FunctionDecl* FD = dyn_cast<FunctionDecl>(ND)) {
          // This decl is a function.
          //
          //  Do function argument matching.
          //
          if (!IsOverload(Context, FuncTemplateArgs, GivenArgTypes, FD,
                          UseMemberUsingDeclRules)) {
            // We have a function argument match.
            if (UseMemberUsingDeclRules && IsUsingDecl) {
              // But it came from a using decl and we are
              // looking up a class member func, ignore it.
              continue;
            }
            TheDecl = dyn_cast<clang::FunctionDecl>(*I);
            break;
          }
        }
      }
    }
    return TheDecl;
  }

  const FunctionDecl* LookupHelper::findFunctionArgs(const Decl* scopeDecl,
                                                       llvm::StringRef funcName,
                                                llvm::StringRef funcArgs) const {
    //
    //  Our return value.
    //
    FunctionDecl* TheDecl = 0;
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor& PP = S.getPreprocessor();
    ASTContext& Context = S.getASTContext();
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    bool OldSuppressAllDiagnostics =
      PP.getDiagnostics().getSuppressAllDiagnostics();
    PP.getDiagnostics().setSuppressAllDiagnostics(true);
    //
    //  Convert the passed decl into a nested name specifier,
    //  a scope spec, and a decl context.
    //
    NestedNameSpecifier* classNNS = 0;
    if (const NamespaceDecl* NSD = dyn_cast<const NamespaceDecl>(scopeDecl)) {
      classNNS = NestedNameSpecifier::Create(Context, 0,
                                             const_cast<NamespaceDecl*>(NSD));
    }
    else if (const RecordDecl* RD = dyn_cast<const RecordDecl>(scopeDecl)) {
      const Type* T = Context.getRecordType(RD).getTypePtr();
      classNNS = NestedNameSpecifier::Create(Context, 0, false, T);
    }
    else if (llvm::isa<TranslationUnitDecl>(scopeDecl)) {
      classNNS = NestedNameSpecifier::GlobalSpecifier(Context);
    }
    else {
      // Not a namespace or class, we cannot use it.
      return 0;
    }
    CXXScopeSpec SS;
    SS.MakeTrivial(Context, classNNS, SourceRange());
    DeclContext* foundDC = dyn_cast<DeclContext>(const_cast<Decl*>(scopeDecl));
    //
    //  Some validity checks on the passed decl.
    //
    if (foundDC->isDependentContext()) {
      // Passed decl is a template, we cannot use it.
      return 0;
    }
    if (S.RequireCompleteDeclContext(SS, foundDC)) {
      // Forward decl or instantiation failure, we cannot use it.
      return 0;
    }
    //
    //  Get ready for arg list parsing.
    //
    llvm::SmallVector<QualType, 4> GivenArgTypes;
    llvm::SmallVector<Expr*, 4> GivenArgs;
    //
    //  If we are looking up a member function, construct
    //  the implicit object argument.
    //
    //  Note: For now this is always a non-CV qualified lvalue.
    //
    QualType ClassType;
    Expr* ObjExpr = 0;
    Expr::Classification ObjExprClassification;
    if (CXXRecordDecl* CRD = dyn_cast<CXXRecordDecl>(foundDC)) {
      ClassType = Context.getTypeDeclType(CRD).getCanonicalType();
      ObjExpr = new (Context) OpaqueValueExpr(SourceLocation(),
        ClassType, VK_LValue);
      ObjExprClassification = ObjExpr->Classify(Context);
      //GivenArgTypes.insert(GivenArgTypes.begin(), ClassType);
      //GivenArgs.insert(GivenArgs.begin(), ObjExpr);
    }
    //
    //  Tell the parser to not attempt spelling correction.
    //
    bool OldSpellChecking = PP.getLangOpts().SpellChecking;
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = 0;
    //
    //  Tell the diagnostic consumer we are switching files.
    //
    DiagnosticConsumer* DClient = S.getDiagnostics().getClient();
    DClient->BeginSourceFile(PP.getLangOpts(), &PP);
    //
    //  Create a fake file to parse the arguments.
    //
    llvm::MemoryBuffer* SB 
      = llvm::MemoryBuffer::getMemBufferCopy(funcArgs.str()
                                             + "\n", "func.args.file");
    FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    bool ResetIncrementalProcessing = false;
    if (!PP.isIncrementalProcessingEnabled()) {
      ResetIncrementalProcessing = true;
      PP.enableIncrementalProcessing();
    }
    //
    //  Switch to the new file the way #include does.
    //
    //  Note: To switch back to the main file we must consume an eof token.
    //
    PP.EnterSourceFile(FID, 0, SourceLocation());
    PP.Lex(const_cast<Token&>(P.getCurToken()));
    //
    //  Setup to reset parser state on exit.
    //
    ParserStateRAII ResetParserState(S, &P, ResetIncrementalProcessing,
                                     OldSuppressAllDiagnostics, 
                                     OldSpellChecking);
    //
    //  Parse the arguments now.
    //
    {
      PrintingPolicy Policy(Context.getPrintingPolicy());
      Policy.SuppressTagKeyword = true;
      Policy.SuppressUnwrittenScope = true;
      Policy.SuppressInitializers = true;
      Policy.AnonymousTagLocations = false;
      std::string proto;
      {
        bool first_time = true;
        while (P.getCurToken().isNot(tok::eof)) {
          ExprResult Res = ParserExt::ParseAssignmentExpressionFwd(&P);
          if (Res.isUsable()) {
            Expr* expr = Res.release();
            GivenArgs.push_back(expr);
            QualType QT = expr->getType().getCanonicalType();
            QualType NonRefQT(QT.getNonReferenceType());
            GivenArgTypes.push_back(NonRefQT);
            if (first_time) {
              first_time = false;
            }
            else {
              proto += ',';
            }
            std::string empty;
            llvm::raw_string_ostream tmp(empty);
            expr->printPretty(tmp, /*PrinterHelper=*/0, Policy, 
                              /*Indentation=*/0);
            proto += tmp.str();
          }
          if (!P.getCurToken().is(tok::comma)) {
            break;
          }
          ParserExt::ConsumeTokenFwd(&P);
        }
      }
    }
    if (P.getCurToken().isNot(tok::eof)) {
      // We did not consume all of the arg list, bad parse.
      return TheDecl;
    }
    {
      //
      //  Cleanup after the arg list parse.
      //
      ParserExt::SkipUntilFwd(&P, clang::tok::eof, /*StopAtSemi*/false,
                              /*DontConsume*/false,
                              /*StopAtCodeCompletion*/false);
      DClient->EndSourceFile();
      S.getDiagnostics().Reset();
      //
      //  Create a fake file to parse the function name.
      //
      {
        llvm::MemoryBuffer* SB 
          = llvm::MemoryBuffer::getMemBufferCopy(funcName.str()
                                                 + "\n", "lookup.funcname.file");
        clang::FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
        PP.EnterSourceFile(FID, /*DirLookup=*/0, clang::SourceLocation());
        PP.Lex(const_cast<clang::Token&>(P.getCurToken()));
      }
      //
      //  Make the class we are looking up the function
      //  in the current scope to please the constructor
      //  name lookup.  We do not need to do this otherwise,
      //  and may be able to remove it in the future if
      //  the way constructors are looked up changes.
      //
      ParserExt::EnterScopeFwd(&P, Scope::DeclScope);
      S.EnterDeclaratorContext(P.getCurScope(), foundDC);
      //
      //  Parse the function name.
      //
      SourceLocation TemplateKWLoc;
      UnqualifiedId FuncId;
      if (ParserExt::ParseUnqualifiedIdFwd(&P, SS, /*EnteringContext*/false,
                                           /*AllowDestructorName*/true,
                                           /*AllowConstructorName*/true,
                                           ParsedType(), TemplateKWLoc, FuncId)){
        // Failed parse, cleanup.
        S.ExitDeclaratorContext(P.getCurScope());
        ParserExt::ExitScopeFwd(&P);
        return TheDecl;
      }
      //
      //  Get any template args in the function name.
      //
      TemplateArgumentListInfo FuncTemplateArgsBuffer;
      DeclarationNameInfo FuncNameInfo;
      const TemplateArgumentListInfo* FuncTemplateArgs;
      S.DecomposeUnqualifiedId(FuncId, FuncTemplateArgsBuffer, FuncNameInfo, 
                               FuncTemplateArgs);
      //
      //  Lookup the function name in the given class now.
      //
      DeclarationName FuncName = FuncNameInfo.getName();
      SourceLocation FuncNameLoc = FuncNameInfo.getLoc();
      LookupResult Result(S, FuncName, FuncNameLoc, Sema::LookupMemberName, 
                          Sema::NotForRedeclaration);
      if (!S.LookupQualifiedName(Result, foundDC)) {
        // Lookup failed.
        // Destroy the scope we created first, and
        // restore the original.
        S.ExitDeclaratorContext(P.getCurScope());
        ParserExt::ExitScopeFwd(&P);
        // Then cleanup and exit.
        return TheDecl;
      }
      //
      //  Destroy the scope we created, and restore the original.
      //
      S.ExitDeclaratorContext(P.getCurScope());
      ParserExt::ExitScopeFwd(&P);
      //
      //  Check for lookup failure.
      //
      if (!(Result.getResultKind() == LookupResult::Found) &&
          !(Result.getResultKind() == LookupResult::FoundOverloaded)) {
        // Lookup failed.
        return TheDecl;
      }
      //
      //  Dump what was found.
      //
      //if (Result.getResultKind() == LookupResult::Found) {
      //  NamedDecl* ND = Result.getFoundDecl();
      //  std::string buf;
      //  llvm::raw_string_ostream tmp(buf);
      //  ND->print(tmp, 0);
      //  fprintf(stderr, "Found: %s\n", tmp.str().c_str());
      //} else if (Result.getResultKind() == LookupResult::FoundOverloaded) {
      //  fprintf(stderr, "Found overload set!\n");
      //  Result.print(llvm::outs());
      //  fprintf(stderr, "\n");
      //}
      {
        //
        //  Construct the overload candidate set.
        //
        OverloadCandidateSet Candidates(FuncNameInfo.getLoc());
        for (LookupResult::iterator I = Result.begin(), E = Result.end();
            I != E; ++I) {
          NamedDecl* ND = *I;
          if (FunctionDecl* FD = dyn_cast<FunctionDecl>(ND)) {
            if (isa<CXXMethodDecl>(FD) &&
                !cast<CXXMethodDecl>(FD)->isStatic() &&
                !isa<CXXConstructorDecl>(FD)) {
              // Class method, not static, not a constructor, so has
              // an implicit object argument.
              CXXMethodDecl* MD = cast<CXXMethodDecl>(FD);
              //{
              //  std::string buf;
              //  llvm::raw_string_ostream tmp(buf);
              //  MD->print(tmp, 0);
              //  fprintf(stderr, "Considering method: %s\n",
              //    tmp.str().c_str());
              //}
              if (FuncTemplateArgs && (FuncTemplateArgs->size() != 0)) {
                // Explicit template args were given, cannot use a plain func.
                //fprintf(stderr, "rejected: template args given\n");
                continue;
              }
              S.AddMethodCandidate(MD, I.getPair(), MD->getParent(),
                                   /*ObjectType=*/ClassType,
                                  /*ObjectClassification=*/ObjExprClassification,
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                   Candidates);
            }
            else {
              //{
              //  std::string buf;
              //  llvm::raw_string_ostream tmp(buf);
              //  FD->print(tmp, 0);
              //  fprintf(stderr, "Considering func: %s\n", tmp.str().c_str());
              //}
              const FunctionProtoType* Proto = dyn_cast<FunctionProtoType>(
                FD->getType()->getAs<clang::FunctionType>());
              if (!Proto) {
                // Function has no prototype, cannot do overloading.
                //fprintf(stderr, "rejected: no prototype\n");
                continue;
              }
              if (FuncTemplateArgs && (FuncTemplateArgs->size() != 0)) {
                // Explicit template args were given, cannot use a plain func.
                //fprintf(stderr, "rejected: template args given\n");
                continue;
              }
              S.AddOverloadCandidate(FD, I.getPair(),
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                     Candidates);
            }
          }
          else if (FunctionTemplateDecl* FTD =
              dyn_cast<FunctionTemplateDecl>(ND)) {
            if (isa<CXXMethodDecl>(FTD->getTemplatedDecl()) &&
                !cast<CXXMethodDecl>(FTD->getTemplatedDecl())->isStatic() &&
                !isa<CXXConstructorDecl>(FTD->getTemplatedDecl())) {
              // Class method template, not static, not a constructor, so has
              // an implicit object argument.
              //{
              //  std::string buf;
              //  llvm::raw_string_ostream tmp(buf);
              //  FTD->print(tmp, 0);
              //  fprintf(stderr, "Considering method template: %s\n",
              //    tmp.str().c_str());
              //}
              S.AddMethodTemplateCandidate(FTD, I.getPair(),
                                      cast<CXXRecordDecl>(FTD->getDeclContext()),
                         const_cast<TemplateArgumentListInfo*>(FuncTemplateArgs),
                                           /*ObjectType=*/ClassType,
                                  /*ObjectClassification=*/ObjExprClassification,
                   llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                                           Candidates);
            }
            else {
              //{
              //  std::string buf;
              //  llvm::raw_string_ostream tmp(buf);
              //  FTD->print(tmp, 0);
              //  fprintf(stderr, "Considering func template: %s\n",
              //    tmp.str().c_str());
              //}
              S.AddTemplateOverloadCandidate(FTD, I.getPair(),
                const_cast<TemplateArgumentListInfo*>(FuncTemplateArgs),
                llvm::makeArrayRef<Expr*>(GivenArgs.data(), GivenArgs.size()),
                Candidates, /*SuppressUserConversions=*/false);
            }
          }
          else {
            //{
            //  std::string buf;
            //  llvm::raw_string_ostream tmp(buf);
            //  FD->print(tmp, 0);
            //  fprintf(stderr, "Considering non-func: %s\n",
            //    tmp.str().c_str());
            //  fprintf(stderr, "rejected: not a function\n");
            //}
          }
        }
        //
        //  Find the best viable function from the set.
        //
        {
          OverloadCandidateSet::iterator Best;
          OverloadingResult OR = Candidates.BestViableFunction(S, 
                                                             Result.getNameLoc(),
                                                               Best);
          if (OR == OR_Success) {
            TheDecl = Best->Function;
          }
        }
      }
      //
      //  Dump the overloading result.
      //
      //if (TheDecl) {
      //  std::string buf;
      //  llvm::raw_string_ostream tmp(buf);
      //  TheDecl->print(tmp, 0);
      //  fprintf(stderr, "Match: %s\n", tmp.str().c_str());
      //  TheDecl->dump();
      //  fprintf(stderr, "\n");
      //}
    }
    return TheDecl;
  }

  void LookupHelper::findArgList(llvm::StringRef argList,
                                 llvm::SmallVector<Expr*, 4>& argExprs) const {
    //
    //  Some utilities.
    //
    // Use P for shortness
    Parser& P = *m_Parser;
    Sema& S = P.getActions();
    Preprocessor &PP = S.getPreprocessor();
    //
    //  Tell the diagnostic engine to ignore all diagnostics.
    //
    bool OldSuppressAllDiagnostics =
      PP.getDiagnostics().getSuppressAllDiagnostics();
    PP.getDiagnostics().setSuppressAllDiagnostics(true);
    //
    //  Tell the parser to not attempt spelling correction.
    //
    bool OldSpellChecking = PP.getLangOpts().SpellChecking;
    const_cast<LangOptions &>(PP.getLangOpts()).SpellChecking = 0;
    //
    //  Tell the diagnostic consumer we are switching files.
    //
    DiagnosticConsumer* DClient = S.getDiagnostics().getClient();
    DClient->BeginSourceFile(PP.getLangOpts(), &PP);
    //
    //  Create a fake file to parse the arguments.
    //
    llvm::MemoryBuffer *SB 
      = llvm::MemoryBuffer::getMemBufferCopy(argList.str()
                                             + "\n", "arg.list.file");
    FileID FID = S.getSourceManager().createFileIDForMemBuffer(SB);
    //
    //  Turn on ignoring of the main file eof token.
    //
    //  Note: We need this because token readahead in the following
    //        routine calls ends up parsing it multiple times.
    //
    bool ResetIncrementalProcessing = false;
    if (!PP.isIncrementalProcessingEnabled()) {
      ResetIncrementalProcessing = true;
      PP.enableIncrementalProcessing();
    }
    //
    //  Switch to the new file the way #include does.
    //
    //  Note: To switch back to the main file we must consume an eof token.
    //
    PP.EnterSourceFile(FID, 0, SourceLocation());
    PP.Lex(const_cast<Token &>(P.getCurToken()));
    //
    //  Setup to reset parser state on exit.
    //
    ParserStateRAII ResetParserState(S, &P, ResetIncrementalProcessing,
                                     OldSuppressAllDiagnostics, 
                                     OldSpellChecking);
    //
    //  Parse the arguments now.
    //
    {
      bool hasUnusableResult = false;
      while (P.getCurToken().isNot(tok::eof)) {
        ExprResult Res = ParserExt::ParseAssignmentExpressionFwd(&P);
        if (Res.isUsable()) {
          argExprs.push_back(Res.release());
        }
        else {
          hasUnusableResult = true;
          break;
        }
        if (!P.getCurToken().is(tok::comma)) {
          break;
        }
        ParserExt::ConsumeTokenFwd(&P);
      }
      if (hasUnusableResult)
        // if one of the arguments is not usable return empty.
        argExprs.clear();
    }
    //
    // Advance the parser to the end of the file, and pop the include stack.
    //
    // Note: Consuming the EOF token will pop the include stack.
    //
    ParserExt::SkipUntilFwd(&P, tok::eof, /*StopAtSemi*/false, 
                            /*DontConsume*/false,
                            /*StopAtCodeCompletion*/false);
    if (ResetIncrementalProcessing) {
      PP.enableIncrementalProcessing(false);
    }
    DClient->EndSourceFile();
    S.getDiagnostics().Reset();
    S.getDiagnostics().setSuppressAllDiagnostics(OldSuppressAllDiagnostics);
    const_cast<LangOptions&>(PP.getLangOpts()).SpellChecking = OldSpellChecking;    
  }



} // end namespace cling