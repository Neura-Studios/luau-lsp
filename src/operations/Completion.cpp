#include "Luau/AstQuery.h"
#include "Luau/Autocomplete.h"
#include "Luau/TypeUtils.h"

#include "LSP/LanguageServer.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/LuauExt.hpp"
#include "LSP/DocumentationParser.hpp"

/// Defining sort text levels assigned to completion items
/// Note that sort text is lexicographically
namespace SortText
{
static constexpr const char* PrioritisedSuggestion = "0";
static constexpr const char* TableProperties = "1";
static constexpr const char* CorrectTypeKind = "2";
static constexpr const char* CorrectFunctionResult = "3";
static constexpr const char* Default = "4";
static constexpr const char* WrongIndexType = "5";
static constexpr const char* MetatableIndex = "6";
static constexpr const char* AutoImports = "7";
static constexpr const char* Keywords = "8";
} // namespace SortText

static constexpr const char* COMMON_SERVICES[] = {
    "Players",
    "ReplicatedStorage",
    "ServerStorage",
    "MessagingService",
    "TeleportService",
    "HttpService",
    "CollectionService",
    "DataStoreService",
    "ContextActionService",
    "UserInputService",
    "Teams",
    "Chat",
    "TextService",
    "TextChatService",
    "GamepadService",
    "VoiceChatService",
};

static constexpr const char* COMMON_INSTANCE_PROPERTIES[] = {
    "Parent",
    "Name",
    // Methods
    "FindFirstChild",
    "IsA",
    "Destroy",
    "GetAttribute",
    "GetChildren",
    "GetDescendants",
    "WaitForChild",
    "Clone",
    "SetAttribute",
};

void WorkspaceFolder::endAutocompletion(const lsp::CompletionParams& params)
{
    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto document = fileResolver.getTextDocument(params.textDocument.uri);
    if (!document)
        return;
    auto position = document->convertPosition(params.position);

    if (frontend.isDirty(moduleName))
        frontend.check(moduleName);

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return;

    auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position);
    if (ancestry.size() < 2)
        return;

    Luau::AstNode* parent = ancestry.at(ancestry.size() - 2);
    if (!parent)
        return;

    // We should only apply it if the line just above us is the start of the unclosed statement
    // Otherwise, we insert ends in weird places if theirs an unclosed stat a while away
    if (!parent->is<Luau::AstStatForIn>() && !parent->is<Luau::AstStatFor>() && !parent->is<Luau::AstStatIf>() && !parent->is<Luau::AstStatWhile>() &&
        !parent->is<Luau::AstExprFunction>())
        return;
    if (params.position.line - parent->location.begin.line > 1)
        return;

    auto unclosedBlock = false;
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
    {
        if (auto* statForIn = (*it)->as<Luau::AstStatForIn>(); statForIn && !statForIn->hasEnd)
            unclosedBlock = true;
        if (auto* statFor = (*it)->as<Luau::AstStatFor>(); statFor && !statFor->hasEnd)
            unclosedBlock = true;
        if (auto* statIf = (*it)->as<Luau::AstStatIf>(); statIf && !statIf->hasEnd)
            unclosedBlock = true;
        if (auto* statWhile = (*it)->as<Luau::AstStatWhile>(); statWhile && !statWhile->hasEnd)
            unclosedBlock = true;
        if (auto* exprFunction = (*it)->as<Luau::AstExprFunction>(); exprFunction && !exprFunction->hasEnd)
            unclosedBlock = true;
    }

    // TODO: we could potentially extend this further that just `hasEnd`
    // by inserting `then`, `until` `do` etc. It seems Studio does this

    if (unclosedBlock)
    {
        // If the position marker is at the very end of the file, if we insert one line further then vscode will
        // not be happy and will insert at the position marker.
        // If its in the middle of the file, vscode won't change the marker
        if (params.position.line == document->lineCount() - 1)
        {
            // Insert an end at the current position, with a newline before it
            // We replace all the current contents of the line since it will just be whitespace
            lsp::TextEdit edit{{{params.position.line, 0}, {params.position.line, params.position.character}}, "\nend\n"};
            std::unordered_map<std::string, std::vector<lsp::TextEdit>> changes{{params.textDocument.uri.toString(), {edit}}};
            client->applyEdit({"insert end", {changes}},
                [this](auto) -> void
                {
                    // Move the cursor up
                    // $/command notification has been manually added by us in the extension
                    client->sendNotification("$/command", std::make_optional<json>({
                                                              {"command", "cursorMove"},
                                                              {"data", {{"to", "prevBlankLine"}}},
                                                          }));
                });
        }
        else
        {
            // Find the indentation level to stick the end on
            std::string indent = "";
            if (document->lineCount() > 1)
            {
                // Use the indentation of the previous line, as thats where the stat begins
                auto prevLine = document->getLine(params.position.line - 1);
                if (prevLine.size() > 0)
                {
                    auto ch = prevLine.at(0);
                    if (ch == ' ' || ch == '\t')
                    {
                        for (auto it = prevLine.begin(); it != prevLine.end(); ++it)
                        {
                            if (*it != ch)
                                break;
                            indent += *it;
                        }
                    }
                }
            }

            // Insert the end onto the next line
            lsp::Position position{params.position.line + 1, 0};
            lsp::TextEdit edit{{position, position}, indent + "end\n"};
            std::unordered_map<std::string, std::vector<lsp::TextEdit>> changes{{params.textDocument.uri.toString(), {edit}}};
            client->applyEdit({"insert end", {changes}});
        }
    }
}

/// Attempts to retrieve a list of service names by inspecting the global type definitions
static std::vector<std::string> getServiceNames(const Luau::ScopePtr& scope)
{
    std::vector<std::string> services{};

    if (auto dataModelType = scope->lookupType("ServiceProvider"))
    {
        if (auto ctv = Luau::get<Luau::ClassType>(dataModelType->type))
        {
            if (auto getService = Luau::lookupClassProp(ctv, "GetService"))
            {
                if (auto itv = Luau::get<Luau::IntersectionType>(getService->type))
                {
                    for (auto part : itv->parts)
                    {
                        if (auto ftv = Luau::get<Luau::FunctionType>(part))
                        {
                            auto it = Luau::begin(ftv->argTypes);
                            auto end = Luau::end(ftv->argTypes);

                            if (it != end && ++it != end)
                            {
                                if (auto stv = Luau::get<Luau::SingletonType>(*it))
                                {
                                    if (auto ss = Luau::get<Luau::StringSingleton>(stv))
                                    {
                                        services.emplace_back(ss->value);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return services;
}

void WorkspaceFolder::suggestImports(
    const Luau::ModuleName& moduleName, const Luau::Position& position, const ClientConfiguration& config, std::vector<lsp::CompletionItem>& result)
{
    auto sourceModule = frontend.getSourceModule(moduleName);
    auto module = frontend.moduleResolverForAutocomplete.getModule(moduleName);
    if (!sourceModule || !module)
        return;

    // If in roblox mode - suggest services
    if (config.types.roblox)
    {
        auto scope = Luau::findScopeAtPosition(*module, position);
        if (!scope)
            return;

        // Place after any hot comments and TODO: already imported services
        size_t minimumLineNumber = 0;
        for (const auto& hotComment : sourceModule->hotcomments)
        {
            if (!hotComment.header)
                continue;
            if (hotComment.location.begin.line >= minimumLineNumber)
                minimumLineNumber = hotComment.location.begin.line + 1U;
        }

        FindServicesVisitor visitor;
        visitor.visit(sourceModule->root);

        if (visitor.firstServiceDefinitionLine)
            minimumLineNumber = *visitor.firstServiceDefinitionLine > minimumLineNumber ? *visitor.firstServiceDefinitionLine : minimumLineNumber;

        auto services = getServiceNames(frontend.typeCheckerForAutocomplete.globalScope);
        for (auto& service : services)
        {
            // ASSUMPTION: if the service was defined, it was defined with the exact same name
            bool isAlreadyDefined = false;
            size_t lineNumber = minimumLineNumber;
            for (auto& [definedService, stat] : visitor.serviceLineMap)
            {
                auto location = stat->location.begin.line;
                if (definedService == service)
                {
                    isAlreadyDefined = true;
                    break;
                }

                if (definedService < service && location >= lineNumber)
                    lineNumber = location + 1;
            }

            if (isAlreadyDefined)
                continue;

            auto importText = "local " + service + " = game:GetService(\"" + service + "\")\n";

            lsp::CompletionItem item;
            item.label = service;
            item.kind = lsp::CompletionItemKind::Class;
            item.detail = "Auto-import";
            item.documentation = {lsp::MarkupKind::Markdown, codeBlock("lua", importText)};
            item.insertText = service;
            item.sortText = SortText::AutoImports;

            lsp::Position placement{lineNumber, 0};
            item.additionalTextEdits.emplace_back(lsp::TextEdit{{placement, placement}, importText});

            result.emplace_back(item);
        }
    }
}

static bool canUseSnippets(const lsp::ClientCapabilities& capabilities)
{
    return capabilities.textDocument && capabilities.textDocument->completion && capabilities.textDocument->completion->completionItem &&
           capabilities.textDocument->completion->completionItem->snippetSupport;
}

std::vector<lsp::CompletionItem> WorkspaceFolder::completion(const lsp::CompletionParams& params)
{
    auto config = client->getConfiguration(rootUri);

    if (!config.completion.enabled)
        return {};

    if (params.context && params.context->triggerCharacter == "\n")
    {
        if (config.autocompleteEnd)
            endAutocompletion(params);
        return {};
    }

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());

    bool isGetService = false;

    auto position = textDocument->convertPosition(params.position);
    auto result = Luau::autocomplete(frontend, moduleName, position,
        [&](const std::string& tag, std::optional<const Luau::ClassType*> ctx,
            std::optional<std::string> contents) -> std::optional<Luau::AutocompleteEntryMap>
        {
            if (tag == "ClassNames")
            {
                if (auto instanceType = frontend.typeChecker.globalScope->lookupType("Instance"))
                {
                    if (auto* ctv = Luau::get<Luau::ClassType>(instanceType->type))
                    {
                        Luau::AutocompleteEntryMap result;
                        for (auto& [_, ty] : frontend.typeChecker.globalScope->exportedTypeBindings)
                        {
                            if (auto* c = Luau::get<Luau::ClassType>(ty.type))
                            {
                                // Check if the ctv is a subclass of instance
                                if (Luau::isSubclass(c, ctv))

                                    result.insert_or_assign(
                                        c->name, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String, frontend.builtinTypes->stringType,
                                                     false, false, Luau::TypeCorrectKind::Correct});
                            }
                        }

                        return result;
                    }
                }
            }
            else if (tag == "Properties")
            {
                if (ctx && ctx.value())
                {
                    Luau::AutocompleteEntryMap result;
                    auto ctv = ctx.value();
                    while (ctv)
                    {
                        for (auto& [propName, _] : ctv->props)
                        {
                            result.insert_or_assign(propName, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String,
                                                                  frontend.builtinTypes->stringType, false, false, Luau::TypeCorrectKind::Correct});
                        }
                        if (ctv->parent)
                            ctv = Luau::get<Luau::ClassType>(*ctv->parent);
                        else
                            break;
                    }
                    return result;
                }
            }
            else if (tag == "Enums")
            {
                auto it = frontend.typeChecker.globalScope->importedTypeBindings.find("Enum");
                if (it == frontend.typeChecker.globalScope->importedTypeBindings.end())
                    return std::nullopt;

                Luau::AutocompleteEntryMap result;
                for (auto& [enumName, _] : it->second)
                    result.insert_or_assign(enumName, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String, frontend.builtinTypes->stringType,
                                                          false, false, Luau::TypeCorrectKind::Correct});

                return result;
            }
            else if (tag == "Require")
            {
                if (!contents.has_value())
                    return std::nullopt;

                try
                {
                    auto contentsString = contents.value();
                    auto currentDirectory = fileResolver.getRequireBasePath(moduleName).append(contentsString);

                    Luau::AutocompleteEntryMap result;
                    for (const auto& dir_entry : std::filesystem::directory_iterator(currentDirectory))
                    {
                        if (dir_entry.is_regular_file() || dir_entry.is_directory())
                        {
                            std::string fileName = dir_entry.path().filename().generic_string();
                            Luau::AutocompleteEntry entry{
                                Luau::AutocompleteEntryKind::String, frontend.builtinTypes->stringType, false, false, Luau::TypeCorrectKind::Correct};
                            entry.tags.push_back(dir_entry.is_directory() ? "Directory" : "File");

                            result.insert_or_assign(fileName, entry);
                        }
                    }

                    // Add in ".." support
                    if (currentDirectory.has_parent_path())
                    {
                        Luau::AutocompleteEntry dotdotEntry{
                            Luau::AutocompleteEntryKind::String, frontend.builtinTypes->stringType, false, false, Luau::TypeCorrectKind::Correct};
                        dotdotEntry.tags.push_back("Directory");
                        result.insert_or_assign("..", dotdotEntry);
                    }

                    return result;
                }
                catch (std::exception&)
                {
                    return std::nullopt;
                }
            }
            else if (tag == "PrioritiseCommonServices")
            {
                // We are autocompleting a `game:GetService("$1")` call, so we set a flag to
                // highlight this so that we can prioritise common services first in the list
                isGetService = true;
            }

            return std::nullopt;
        });


    std::vector<lsp::CompletionItem> items{};

    for (auto& [name, entry] : result.entryMap)
    {
        lsp::CompletionItem item;
        item.label = name;
        item.deprecated = entry.deprecated;
        item.sortText = SortText::Default;

        // Handle documentation
        std::optional<std::string> documentationString = std::nullopt;
        if (std::optional<std::string> docs;
            entry.documentationSymbol && (docs = printDocumentation(client->documentation, *entry.documentationSymbol)) && docs)
            documentationString = *docs;
        else if (entry.type.has_value())
            documentationString = getDocumentationForType(entry.type.value());
        // TODO: Handle documentation on properties
        if (documentationString)
            item.documentation = {lsp::MarkupKind::Markdown, documentationString.value()};

        if (entry.wrongIndexType)
            item.sortText = SortText::WrongIndexType;
        if (entry.typeCorrect == Luau::TypeCorrectKind::Correct)
            item.sortText = SortText::CorrectTypeKind;
        else if (entry.typeCorrect == Luau::TypeCorrectKind::CorrectFunctionResult)
            item.sortText = SortText::CorrectFunctionResult;
        else if (entry.kind == Luau::AutocompleteEntryKind::Property && types::isMetamethod(name))
            item.sortText = SortText::MetatableIndex;
        else if (entry.kind == Luau::AutocompleteEntryKind::Property)
            item.sortText = SortText::TableProperties;
        else if (entry.kind == Luau::AutocompleteEntryKind::Keyword)
            item.sortText = SortText::Keywords;

        // If its a `game:GetSerivce("$1")` call, then prioritise common services
        if (isGetService)
        {
            if (auto it = std::find(std::begin(COMMON_SERVICES), std::end(COMMON_SERVICES), name); it != std::end(COMMON_SERVICES))
                item.sortText = SortText::PrioritisedSuggestion;
        }
        // If calling a property on an Instance, then prioritise these properties
        if (auto instanceType = frontend.typeCheckerForAutocomplete.globalScope->lookupType("Instance");
            instanceType && Luau::get<Luau::ClassType>(instanceType->type) && entry.containingClass &&
            Luau::isSubclass(entry.containingClass.value(), Luau::get<Luau::ClassType>(instanceType->type)) && !entry.wrongIndexType)
        {
            if (auto it = std::find(std::begin(COMMON_INSTANCE_PROPERTIES), std::end(COMMON_INSTANCE_PROPERTIES), name);
                it != std::end(COMMON_INSTANCE_PROPERTIES))
                item.sortText = SortText::PrioritisedSuggestion;
        }

        switch (entry.kind)
        {
        case Luau::AutocompleteEntryKind::Property:
            item.kind = lsp::CompletionItemKind::Field;
            break;
        case Luau::AutocompleteEntryKind::Binding:
            item.kind = lsp::CompletionItemKind::Variable;
            break;
        case Luau::AutocompleteEntryKind::Keyword:
            item.kind = lsp::CompletionItemKind::Keyword;
            break;
        case Luau::AutocompleteEntryKind::String:
            item.kind = lsp::CompletionItemKind::Constant; // TODO: is a string autocomplete always a singleton constant?
            break;
        case Luau::AutocompleteEntryKind::Type:
            item.kind = lsp::CompletionItemKind::Interface;
            break;
        case Luau::AutocompleteEntryKind::Module:
            item.kind = lsp::CompletionItemKind::Module;
            break;
        }

        // Special cases: Files and directory
        if (std::find(entry.tags.begin(), entry.tags.end(), "File") != entry.tags.end())
        {
            item.kind = lsp::CompletionItemKind::File;
            // We shouldn't include the extension when inserting
            std::string insertText = name;
            insertText.erase(insertText.find_last_of('.'));
            item.insertText = insertText;
        }
        else if (std::find(entry.tags.begin(), entry.tags.end(), "Directory") != entry.tags.end())
            item.kind = lsp::CompletionItemKind::Folder;

        // Handle if name is not an identifier
        if (entry.kind == Luau::AutocompleteEntryKind::Property && !Luau::isIdentifier(name))
        {
            auto lastAst = result.ancestry.back();
            if (auto indexName = lastAst->as<Luau::AstExprIndexName>())
            {
                lsp::TextEdit textEdit;
                textEdit.newText = "[\"" + name + "\"]";
                textEdit.range = {
                    textDocument->convertPosition(indexName->indexLocation.begin), textDocument->convertPosition(indexName->indexLocation.end)};
                item.textEdit = textEdit;

                // For some reason, the above text edit can't handle replacing the index operator
                // Hence we remove it using an additional text edit
                item.additionalTextEdits.emplace_back(lsp::TextEdit{
                    {textDocument->convertPosition(indexName->opPosition), {indexName->opPosition.line, indexName->opPosition.column + 1U}}, ""});
            }
        }

        // Handle parentheses suggestions
        if (config.completion.addParentheses)
        {
            if (canUseSnippets(client->capabilities))
            {
                if (entry.parens == Luau::ParenthesesRecommendation::CursorAfter)
                {
                    if (item.textEdit)
                        item.textEdit->newText += "()$0";
                    else
                        item.insertText = name + "()$0";
                    item.insertTextFormat = lsp::InsertTextFormat::Snippet;
                }
                else if (entry.parens == Luau::ParenthesesRecommendation::CursorInside)
                {
                    std::string parenthesesSnippet = config.completion.addTabstopAfterParentheses ? "($1)$0" : "($0)";

                    if (item.textEdit)
                        item.textEdit->newText += parenthesesSnippet;
                    else
                        item.insertText = name + parenthesesSnippet;
                    item.insertTextFormat = lsp::InsertTextFormat::Snippet;
                    // Trigger Signature Help
                    item.command = lsp::Command{"Trigger Signature Help", "editor.action.triggerParameterHints"};
                }
            }
            else
            {
                // We don't support snippets, so just add parentheses
                if (entry.parens == Luau::ParenthesesRecommendation::CursorAfter || entry.parens == Luau::ParenthesesRecommendation::CursorInside)
                {
                    if (item.textEdit)
                        item.textEdit->newText += "()";
                    else
                        item.insertText = name + "()";
                }
            }
        }

        if (entry.type.has_value())
        {
            auto id = Luau::follow(entry.type.value());
            if (Luau::isOverloadedFunction(id))
                item.kind = lsp::CompletionItemKind::Function;

            // Try to infer more type info about the entry to provide better suggestion info
            if (auto ftv = Luau::get<Luau::FunctionType>(id))
            {
                item.kind = lsp::CompletionItemKind::Function;

                // Add label details
                // We also create a better detailed parentheses snippet if we are filling arguments
                std::string detail = "(";
                std::string parenthesesSnippet = "(";

                bool comma = false;
                size_t argIndex = 0;
                size_t snippetIndex = 1;

                auto [minCount, _] = Luau::getParameterExtents(Luau::TxnLog::empty(), ftv->argTypes, true);

                auto it = Luau::begin(ftv->argTypes);
                for (; it != Luau::end(ftv->argTypes); ++it, ++argIndex)
                {
                    std::string argName = "_";
                    if (argIndex < ftv->argNames.size() && ftv->argNames.at(argIndex))
                        argName = ftv->argNames.at(argIndex)->name;

                    // TODO: hasSelf is not always specified, so we manually check for the "self" name (https://github.com/Roblox/luau/issues/551)
                    if (argIndex == 0 && (ftv->hasSelf || argName == "self"))
                        continue;

                    // If the rest of the arguments are optional, don't include in filled call arguments
                    bool includeParensSnippet = argIndex < minCount;

                    if (comma)
                    {
                        detail += ", ";
                        if (includeParensSnippet)
                            parenthesesSnippet += ", ";
                    }

                    detail += argName;
                    if (includeParensSnippet)
                        parenthesesSnippet += "${" + std::to_string(snippetIndex) + ":" + argName + "}";

                    comma = true;
                    snippetIndex++;
                }

                if (auto tail = it.tail())
                {
                    if (comma)
                    {
                        detail += ", ";
                    }
                    detail += Luau::toString(*tail);
                }

                detail += ")";
                parenthesesSnippet += ")";
                item.labelDetails = {detail};

                // If we had CursorAfter, then the function call would not have any arguments
                if (canUseSnippets(client->capabilities) && config.completion.addParentheses && config.completion.fillCallArguments &&
                    entry.parens != Luau::ParenthesesRecommendation::None)
                {
                    if (config.completion.addTabstopAfterParentheses)
                        parenthesesSnippet += "$0";

                    if (item.textEdit)
                        item.textEdit->newText += parenthesesSnippet;
                    else
                        item.insertText = name + parenthesesSnippet;

                    item.insertTextFormat = lsp::InsertTextFormat::Snippet;
                    // Trigger Signature Help
                    item.command = lsp::Command{"Trigger Signature Help", "editor.action.triggerParameterHints"};
                }

                // Add documentation
                if (!entry.documentationSymbol && ftv->definition && ftv->definition->definitionModuleName)
                {
                    item.documentation = {lsp::MarkupKind::Markdown,
                        printMoonwaveDocumentation(getComments(ftv->definition->definitionModuleName.value(), ftv->definition->definitionLocation))};
                }
            }
            else if (auto ttv = Luau::get<Luau::TableType>(id))
            {
                // Special case the RBXScriptSignal type as a connection
                if (ttv->name && ttv->name.value() == "RBXScriptSignal")
                {
                    item.kind = lsp::CompletionItemKind::Event;
                }
            }
            else if (Luau::get<Luau::ClassType>(id))
            {
                item.kind = lsp::CompletionItemKind::Class;
            }
            item.detail = Luau::toString(id);
        }

        items.emplace_back(item);
    }

    if (config.completion.suggestImports &&
        (result.context == Luau::AutocompleteContext::Expression || result.context == Luau::AutocompleteContext::Statement))
    {
        suggestImports(moduleName, position, config, items);
    }

    return items;
}

std::vector<lsp::CompletionItem> LanguageServer::completion(const lsp::CompletionParams& params)
{
    auto workspace = findWorkspace(params.textDocument.uri);
    return workspace->completion(params);
}
