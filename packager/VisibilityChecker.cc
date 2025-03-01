#include "packager/VisibilityChecker.h"
#include "ast/treemap/treemap.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "common/formatting.h"
#include "common/sort.h"
#include "core/Context.h"
#include "core/errors/packager.h"
#include "core/errors/resolver.h"
#include <iterator>

using namespace std::literals::string_view_literals;

namespace sorbet::packager {

namespace {

// For each __package.rb file, traverse the resolved tree and apply the visibility annotations to the symbols.
class PropagateVisibility final {
    const core::packages::PackageInfo &package;

    bool definedByThisPackage(const core::GlobalState &gs, core::ClassOrModuleRef sym) {
        auto pkg = gs.packageDB().getPackageNameForFile(sym.data(gs)->loc().file());
        return this->package.mangledName() == pkg;
    }

    void recursiveExportSymbol(core::GlobalState &gs, bool firstSymbol, core::ClassOrModuleRef klass) {
        // There is a loop between a class and its singleton, and this is the easiest way to detect it.
        if (!firstSymbol && klass.data(gs)->flags.isExported) {
            return;
        }

        // We only mark symbols from this package.
        if (!this->definedByThisPackage(gs, klass)) {
            return;
        }

        klass.data(gs)->flags.isExported = true;

        for (const auto &child : klass.data(gs)->members()) {
            if (child.second.isClassOrModule()) {
                recursiveExportSymbol(gs, false, child.second.asClassOrModuleRef());
            } else if (child.second.isFieldOrStaticField()) {
                child.second.asFieldRef().data(gs)->flags.isExported = true;
            }
        }
    }

    void exportParentNamespace(core::GlobalState &gs, core::ClassOrModuleRef owner) {
        while (owner.exists() && !owner.data(gs)->flags.isExported && this->definedByThisPackage(gs, owner)) {
            owner.data(gs)->flags.isExported = true;
            owner = owner.data(gs)->owner;
        }
    }

    // Lookup the package name on the given root symbol, and mark the final symbol as exported.
    void exportRoot(core::GlobalState &gs, core::ClassOrModuleRef sym) {
        // For a package named `A::B`, the ClassDef that we see in this pass is for a symbol named
        // `<PackageSpecRegistry>::A::B`. In order to make the name `A::B` visibile to packages that have imported
        // `A::B`, we explicitly lookup and export them here. This is a design decision inherited from the previous
        // packages implementation, and we could remove it after migrating Stripe's codebase to not depend on package
        // names being exported by default.
        for (auto name : this->package.fullName()) {
            auto next = sym.data(gs)->findMember(gs, name);
            if (!next.exists() || !next.isClassOrModule()) {
                sym = core::Symbols::noClassOrModule();
                return;
            }

            sym = next.asClassOrModuleRef();
        }

        sym.data(gs)->flags.isExported = true;
    }

    // Mark both `::A::B` and `::Test::A::B` as exported (given package `A::B`).
    // This will terminate the recursive exporting in `exportParentNamespace`, as it stops when it
    // hits either the root or an exported symbol.
    void exportPackageRoots(core::GlobalState &gs) {
        this->exportRoot(gs, core::Symbols::root());

        auto test = core::Symbols::root().data(gs)->findMember(gs, core::Names::Constants::Test());
        if (test.exists() && test.isClassOrModule()) {
            this->exportRoot(gs, test.asClassOrModuleRef());
        }
    }

    // While processing the ClassDef for the package, which will be named something like `<PackageSpecRegistry>::A::B`,
    // we also check that the symbols `A::B` and `Test::A::B` have locations whose package matches the one we're
    // processing. If they don't match, we add locs to ensure that those symbols are associated with this package.
    //
    // The reason for this step is that it's currently allowed to refer to the name of the package outside of the
    // context of the package spec, even if it doesn't explicitly export its top-level name. So in the case above, there
    // would be no `export A::B` line in the package spec. However, if there is a package that is defined in the child
    // namespace of `A::B`, and it at some point has a declaration of the form `module A::B`, the file that contains
    // that declaration will be marked as owning `A::B`, thus breaking the invariant that the file can be used to
    // determine the package that owns a symbol. So, to avoid this case we ensure that the symbols that correspond to
    // the package name are always owned by the package that defines them.
    void setPackageLocs(core::MutableContext ctx, core::LocOffsets loc, core::ClassOrModuleRef sym) {
        std::vector<core::NameRef> names;

        while (sym.exists() && sym != core::Symbols::PackageSpecRegistry()) {
            // The symbol isn't a package name if it's defined outside of the package registry.
            if (sym == core::Symbols::root()) {
                return;
            }

            names.emplace_back(sym.data(ctx)->name);
            sym = sym.data(ctx)->owner;
        }

        absl::c_reverse(names);

        auto &db = ctx.state.packageDB();

        {
            auto packageSym = core::Symbols::root();
            for (auto name : names) {
                auto member = packageSym.data(ctx)->findMember(ctx, name);
                if (!member.exists() || !member.isClassOrModule()) {
                    packageSym = core::Symbols::noClassOrModule();
                    break;
                }

                packageSym = member.asClassOrModuleRef();
            }

            if (packageSym.exists()) {
                auto file = packageSym.data(ctx)->loc().file();
                if (db.getPackageNameForFile(file) != this->package.mangledName()) {
                    packageSym.data(ctx)->addLoc(ctx, ctx.locAt(loc));
                }
            }
        }

        {
            auto testSym = core::Symbols::root();
            auto member = testSym.data(ctx)->findMember(ctx, core::Names::Constants::Test());
            if (!member.exists() || !member.isClassOrModule()) {
                return;
            }

            testSym = member.asClassOrModuleRef();
            for (auto name : names) {
                auto member = testSym.data(ctx)->findMember(ctx, name);
                if (!member.exists() || !member.isClassOrModule()) {
                    testSym = core::Symbols::noClassOrModule();
                    break;
                }

                testSym = member.asClassOrModuleRef();
            }

            if (testSym.exists()) {
                auto file = testSym.data(ctx)->loc().file();
                if (db.getPackageNameForFile(file) != this->package.mangledName()) {
                    testSym.data(ctx)->addLoc(ctx, ctx.locAt(loc));
                }
            }
        }
    }

    // Checks that the package that a symbol is defined in can be exported from the package we're currently checking.
    void checkExportPackage(core::MutableContext &ctx, core::LocOffsets loc, core::SymbolRef sym) {
        auto symPackage = ctx.state.packageDB().getPackageNameForFile(sym.loc(ctx).file());
        if (symPackage != this->package.mangledName()) {
            if (auto e = ctx.beginError(loc, core::errors::Packager::InvalidExport)) {
                e.setHeader("Cannot export `{}` because it is owned by another package", sym.show(ctx));
                e.addErrorLine(sym.loc(ctx), "Defined here");
            }
        }
    }

    PropagateVisibility(const core::packages::PackageInfo &package) : package{package} {}

public:
    // Find uses of export and mark the symbols they mention as exported.
    ast::ExpressionPtr postTransformSend(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (send.fun != core::Names::export_()) {
            return tree;
        }

        if (send.numPosArgs() != 1) {
            // an error will have been raised in the packager pass
            return tree;
        }

        auto *lit = ast::cast_tree<ast::ConstantLit>(send.getPosArg(0));
        if (lit == nullptr || lit->symbol == core::Symbols::StubModule()) {
            // We don't raise an explicit error here, as this is one of two cases:
            //   1. Export is given a non-constant argument
            //   2. The argument failed to resolve
            // In both cases, errors will be raised by previous passes.
            return tree;
        }

        if (lit->symbol.isClassOrModule()) {
            auto sym = lit->symbol.asClassOrModuleRef();
            checkExportPackage(ctx, send.loc, lit->symbol);
            recursiveExportSymbol(ctx, true, sym);

            // When exporting a symbol, we also export its parent namespace. This is a bit of a hack, and it would be
            // great to remove this, but this was the behavior of the previous packager implementation.
            exportParentNamespace(ctx, sym.data(ctx)->owner);
        } else if (lit->symbol.isFieldOrStaticField()) {
            auto sym = lit->symbol.asFieldRef();
            checkExportPackage(ctx, send.loc, lit->symbol);
            sym.data(ctx)->flags.isExported = true;

            // When exporting a field, we also export its parent namespace. This is a bit of a hack, and it would be
            // great to remove this, but this was the behavior of the previous packager implementation.
            exportParentNamespace(ctx, sym.data(ctx)->owner);
        } else {
            std::string_view kind = ""sv;
            switch (lit->symbol.kind()) {
                case core::SymbolRef::Kind::ClassOrModule:
                case core::SymbolRef::Kind::FieldOrStaticField:
                    ENFORCE(false, "ClassOrModule and FieldOrStaticField marked not exportable");
                    break;

                case core::SymbolRef::Kind::Method:
                    kind = "type argument"sv;
                    break;

                case core::SymbolRef::Kind::TypeArgument:
                    kind = "type argument"sv;
                    break;

                case core::SymbolRef::Kind::TypeMember:
                    kind = "type member"sv;
                    break;
            }

            if (auto e = ctx.beginError(send.loc, core::errors::Packager::InvalidExport)) {
                e.setHeader("Only classes, modules, or constants may be exported");
                e.addErrorLine(lit->symbol.loc(ctx), "Defined here");
                e.addErrorNote("`{}` is a `{}`", lit->symbol.show(ctx), kind);
            }
        }

        return tree;
    }

    ast::ExpressionPtr preTransformClassDef(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        if (original.symbol == core::Symbols::root()) {
            return tree;
        }

        ENFORCE(original.symbol != core::Symbols::todo());
        setPackageLocs(ctx, original.name.loc(), original.symbol);

        return tree;
    }

    static ast::ParsedFile run(core::GlobalState &gs, ast::ParsedFile f) {
        if (!f.file.data(gs).isPackage()) {
            return f;
        }

        auto pkgName = gs.packageDB().getPackageNameForFile(f.file);
        if (!pkgName.exists()) {
            return f;
        }

        const auto &package = gs.packageDB().getPackageInfo(pkgName);
        ENFORCE(package.exists(), "Package is associated with a file, but doesn't exist");

        PropagateVisibility pass{package};

        pass.exportPackageRoots(gs);

        core::MutableContext ctx{gs, core::Symbols::root(), f.file};
        f.tree = ast::TreeMap::apply(ctx, pass, std::move(f.tree));

        return f;
    }
};

class VisibilityCheckerPass final {
    bool nameMatchesPackage(const std::vector<core::NameRef> &revNameParts) {
        const auto &packageName = this->package.fullName();

        if (revNameParts.size() < packageName.size()) {
            return false;
        }

        return std::equal(packageName.begin(), packageName.end(), revNameParts.rbegin());
    }

public:
    const core::packages::PackageInfo &package;
    const bool insideTestFile;

    VisibilityCheckerPass(core::Context ctx, const core::packages::PackageInfo &package)
        : package{package}, insideTestFile{ctx.file.data(ctx).isPackagedTest()} {}

    // `keep-def` will reference constants in a way that looks like a packaging violation, but is actually fine. This
    // boolean allows for an early exit when we know we're in the context of processing one of these sends. Currently
    // the only sends that we process this way will not have any nested method calls, but if that changes this will need
    // to become a stack.
    bool ignoreConstant = false;

    ast::ExpressionPtr preTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        ENFORCE(!this->ignoreConstant, "keepForIde has nested sends");
        this->ignoreConstant = send.fun == core::Names::keepForIde();
        return tree;
    }

    ast::ExpressionPtr postTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
        this->ignoreConstant = false;
        return tree;
    }

    ast::ExpressionPtr postTransformConstantLit(core::Context ctx, ast::ExpressionPtr tree) {
        if (this->ignoreConstant) {
            return tree;
        }

        auto &lit = ast::cast_tree_nonnull<ast::ConstantLit>(tree);
        if (!lit.symbol.isClassOrModule() && !lit.symbol.isFieldOrStaticField()) {
            return tree;
        }

        auto loc = lit.symbol.loc(ctx);

        auto otherFile = loc.file();
        if (!otherFile.exists() || !otherFile.data(ctx).isPackaged()) {
            return tree;
        }

        // If the imported symbol comes from the test namespace, we must also be in the test namespace.
        if (otherFile.data(ctx).isPackagedTest() && !this->insideTestFile) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Packager::UsedTestOnlyName)) {
                e.setHeader("`{}` is defined in a test namespace and cannot be referenced in a non-test file",
                            lit.symbol.show(ctx));
            }
        }

        auto &db = ctx.state.packageDB();

        // no need to check visibility for these cases
        auto otherPackage = db.getPackageNameForFile(otherFile);
        if (!otherPackage.exists() || this->package.mangledName() == otherPackage) {
            return tree;
        }

        bool isExported = false;
        if (lit.symbol.isClassOrModule()) {
            isExported = lit.symbol.asClassOrModuleRef().data(ctx)->flags.isExported;
        } else if (lit.symbol.isFieldOrStaticField()) {
            isExported = lit.symbol.asFieldRef().data(ctx)->flags.isExported;
        }

        // Did we use a constant that wasn't exported?
        if (!isExported) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Packager::UsedPackagePrivateName)) {
                auto &pkg = ctx.state.packageDB().getPackageInfo(otherPackage);
                e.setHeader("`{}` resolves but is not exported from `{}`", lit.symbol.show(ctx), pkg.show(ctx));
                auto definedHereLoc = lit.symbol.loc(ctx);
                if (definedHereLoc.file().data(ctx).isRBI()) {
                    e.addErrorSection(core::ErrorSection(
                        core::ErrorColors::format(
                            "Consider marking this RBI file `{}` if it is meant to declare unpackaged constants",
                            "# packaged: false"),
                        {core::ErrorLine(definedHereLoc, "")}));
                } else {
                    e.addErrorLine(definedHereLoc, "Defined here");
                }
                if (auto exp = pkg.addExport(ctx, lit.symbol)) {
                    e.addAutocorrect(std::move(exp.value()));
                }
                if (!db.errorHint().empty()) {
                    e.addErrorNote("{}", db.errorHint());
                }
            }

            return tree;
        }

        auto importType = this->package.importsPackage(otherPackage);
        if (!importType.has_value()) {
            // We failed to import the package that defines the symbol
            if (auto e = ctx.beginError(lit.loc, core::errors::Packager::MissingImport)) {
                auto &pkg = ctx.state.packageDB().getPackageInfo(otherPackage);
                e.setHeader("`{}` resolves but its package is not imported", lit.symbol.show(ctx));
                bool isTestImport = otherFile.data(ctx).isPackagedTest();
                e.addErrorLine(pkg.declLoc(), "Exported from package here");
                if (auto exp = this->package.addImport(ctx, pkg, isTestImport)) {
                    e.addAutocorrect(std::move(exp.value()));
                }

                if (!ctx.file.data(ctx).isPackaged()) {
                    e.addErrorNote(
                        "A `{}` file is allowed to define constants outside of the package's namespace,\n    "
                        "but must still respect its enclosing package's imports.",
                        "# packaged: false");
                }

                if (!db.errorHint().empty()) {
                    e.addErrorNote("{}", db.errorHint());
                }
            }
        } else if (*importType == core::packages::ImportType::Test && !this->insideTestFile) {
            // We used a symbol from a `test_import` in a non-test context
            if (auto e = ctx.beginError(lit.loc, core::errors::Packager::UsedTestOnlyName)) {
                e.setHeader("Used `{}` constant `{}` in non-test file", "test_import", lit.symbol.show(ctx));
                auto &pkg = ctx.state.packageDB().getPackageInfo(otherPackage);
                if (auto exp = this->package.addImport(ctx, pkg, false)) {
                    e.addAutocorrect(std::move(exp.value()));
                }
                e.addErrorLine(pkg.declLoc(), "Defined here");
            }
        }

        return tree;
    }

    ast::ExpressionPtr preTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::ClassDef>(tree);
        if (original.kind == ast::ClassDef::Kind::Class && !original.ancestors.empty()) {
            auto &superClass = original.ancestors[0];
            superClass = ast::TreeMap::apply(ctx, *this, std::move(superClass));
        }

        return tree;
    }

    static std::vector<ast::ParsedFile> run(const core::GlobalState &gs, WorkerPool &workers,
                                            std::vector<ast::ParsedFile> files) {
        Timer timeit(gs.tracer(), "visibility_checker.check_visibility");
        auto resultq = std::make_shared<BlockingBoundedQueue<ast::ParsedFile>>(files.size());
        auto fileq = std::make_shared<ConcurrentBoundedQueue<ast::ParsedFile>>(files.size());

        for (auto &file : files) {
            fileq->push(std::move(file), 1);
        }
        files.clear();

        workers.multiplexJob("VisibilityChecker", [&gs, fileq, resultq]() {
            ast::ParsedFile f;
            for (auto result = fileq->try_pop(f); !result.done(); result = fileq->try_pop(f)) {
                if (!f.file.data(gs).isPackage()) {
                    auto pkgName = gs.packageDB().getPackageNameForFile(f.file);
                    if (pkgName.exists()) {
                        core::Context ctx{gs, core::Symbols::root(), f.file};
                        VisibilityCheckerPass pass{ctx, gs.packageDB().getPackageInfo(pkgName)};
                        f.tree = ast::TreeMap::apply(ctx, pass, std::move(f.tree));
                    }
                }

                resultq->push(std::move(f), 1);
            }
        });

        ast::ParsedFile threadResult;
        for (auto result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs.tracer());
             !result.done();
             result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs.tracer())) {
            files.emplace_back(std::move(threadResult));
        }

        fast_sort(files, [](const auto &a, const auto &b) -> bool { return a.file < b.file; });

        return files;
    }
};

class ImportCheckerPass final {
    struct Import {
        core::SymbolRef package;
        core::LocOffsets importLoc;

        Import(core::SymbolRef package, core::LocOffsets importLoc) : package{package}, importLoc{importLoc} {}

        // Lexicographic ordering, grouping like packages together and ordering them by position in the file.
        bool operator<(const Import &other) const {
            if (this->package == other.package) {
                return this->importLoc.beginPos() < other.importLoc.beginPos();
            }

            return this->package.rawId() < other.package.rawId();
        }
    };

    std::vector<Import> imports;

public:
    ast::ExpressionPtr postTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (send.fun != core::Names::import() && send.fun != core::Names::test_import()) {
            return tree;
        }

        if (send.numPosArgs() != 1) {
            // an error will have been raised in the packager pass
            return tree;
        }

        auto *lit = ast::cast_tree<ast::ConstantLit>(send.getPosArg(0));
        if (lit == nullptr) {
            // We don't raise an explicit error here, for the same reasons as in PropagateVisibility::postTransformSend.
            return tree;
        }

        if (lit->symbol == core::Symbols::StubModule()) {
            // An error was already reported in resolver when the StubModule was created.
            return tree;
        }

        this->imports.emplace_back(lit->symbol, send.loc);

        return tree;
    }

    void checkImports(core::Context ctx) {
        fast_sort(this->imports);

        auto it = this->imports.begin();
        while (true) {
            it = std::adjacent_find(it, this->imports.end(), [](auto &l, auto &r) { return l.package == r.package; });
            if (it == this->imports.end()) {
                break;
            }

            // find the end of the region of duplicated imports
            auto end =
                std::find_if_not(it, this->imports.end(), [sym = it->package](auto &e) { return sym == e.package; });

            auto first = it;

            // Treat the first import as the authoritative one, and report errors for the rest.
            std::advance(it, 1);

            for (; it != end; ++it) {
                if (auto e = ctx.beginError(it->importLoc, core::errors::Packager::InvalidImportOrExport)) {
                    e.setHeader("Duplicate package import `{}`", it->package.show(ctx));
                    e.addErrorLine(ctx.locAt(first->importLoc), "Previous package import found here");
                    e.replaceWith("Remove import", ctx.locAt(it->importLoc), "");
                }
            }
        }

        this->imports.clear();
    }

    static std::vector<ast::ParsedFile> run(const core::GlobalState &gs, WorkerPool &workers,
                                            std::vector<ast::ParsedFile> files) {
        Timer timeit(gs.tracer(), "visibility_checker.check_imports");
        auto resultq = std::make_shared<BlockingBoundedQueue<ast::ParsedFile>>(files.size());
        auto fileq = std::make_shared<ConcurrentBoundedQueue<ast::ParsedFile>>(files.size());

        for (auto &file : files) {
            fileq->push(std::move(file), 1);
        }
        files.clear();

        workers.multiplexJob("ImportChecker", [&gs, fileq, resultq]() {
            ast::ParsedFile f;
            ImportCheckerPass pass;

            for (auto result = fileq->try_pop(f); !result.done(); result = fileq->try_pop(f)) {
                if (f.file.data(gs).isPackage()) {
                    auto pkgName = gs.packageDB().getPackageNameForFile(f.file);
                    if (pkgName.exists()) {
                        core::Context ctx{gs, core::Symbols::root(), f.file};
                        f.tree = ast::TreeMap::apply(ctx, pass, std::move(f.tree));
                        pass.checkImports(ctx);
                    }
                }

                resultq->push(std::move(f), 1);
            }
        });

        ast::ParsedFile threadResult;
        for (auto result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs.tracer());
             !result.done();
             result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs.tracer())) {
            files.emplace_back(std::move(threadResult));
        }

        fast_sort(files, [](const auto &a, const auto &b) -> bool { return a.file < b.file; });

        return files;
    }
};

} // namespace

std::vector<ast::ParsedFile> VisibilityChecker::run(core::GlobalState &gs, WorkerPool &workers,
                                                    std::vector<ast::ParsedFile> files) {
    Timer timeit(gs.tracer(), "visibility_checker.run");

    {
        Timer timeit(gs.tracer(), "visibility_checker.propagate_visibility");
        for (auto &f : files) {
            f = PropagateVisibility::run(gs, std::move(f));
        }
    }

    // We could dispatch to this pass while running the `VisibilityCheckerPass` when we encounter a package file, but
    // the separation of the two is nice for simplifying `runIncremental`.
    files = ImportCheckerPass::run(gs, workers, std::move(files));

    return VisibilityCheckerPass::run(gs, workers, std::move(files));
}

std::vector<ast::ParsedFile> VisibilityChecker::runIncremental(const core::GlobalState &gs, WorkerPool &workers,
                                                               std::vector<ast::ParsedFile> files) {
    Timer timeit(gs.tracer(), "visibility_checker.runIncremental");
    return VisibilityCheckerPass::run(gs, workers, std::move(files));
}

} // namespace sorbet::packager
