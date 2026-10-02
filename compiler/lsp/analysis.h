#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "json.h"

// One file of the game to analyse.
typedef struct analysis_file {
    const char *uri;  // As the editor knows it
    const char *path; // For messages
    const char *text;
    size_t len;
} analysis_file;

// The compiler's front end run on a whole game, for the language server:
// tolerant lexing and parsing of every file, then the checker. Results stay
// valid until the next analysis_run, which resets the compiler's arena. The
// files must outlive the results.
void analysis_run(const analysis_file *files, int count);

// Picks the document the requests below are about. False if it isn't part of
// the analysed game.
bool analysis_select(const char *uri);
int analysis_file_count(void);
const char *analysis_file_uri(int file);
const char *analysis_file_path(int file);

// Diagnostics for one of the game's files.
void analysis_diagnostics(int file, jbuf *out);                              // Diagnostic[]

// LSP results about the selected document, written as JSON. Positions are
// LSP's: 0-based lines and UTF-16 columns. Results can point into other files
// of the game (definitions, references, renames).
void analysis_completion(int line, int character, jbuf *out);                // CompletionList
void analysis_hover(int line, int character, jbuf *out);                     // Hover or null
void analysis_definition(const char *uri, int line, int character, jbuf *out); // Location or null
void analysis_type_definition(int line, int character, jbuf *out);           // Location or null
void analysis_implementation(int line, int character, jbuf *out);            // Location[] or null
// The call hierarchy. Its calls are an item's: the position of its name, its selectionRange.
void analysis_prepare_call_hierarchy(int line, int character, jbuf *out);    // CallHierarchyItem[] or null
void analysis_incoming_calls(int line, int character, jbuf *out);            // CallHierarchyIncomingCall[]
void analysis_outgoing_calls(int line, int character, jbuf *out);            // CallHierarchyOutgoingCall[]
void analysis_symbols(jbuf *out);                                            // DocumentSymbol[]
// SymbolInformation items of the whole game, comma-separated after `written` others.
void analysis_workspace_symbols(const char *query, jbuf *out, int *written);
void analysis_inlay_hints(int start_line, int end_line, jbuf *out);          // InlayHint[]
void analysis_folding_ranges(jbuf *out);                                     // FoldingRange[]
void analysis_semantic_tokens(jbuf *out);                                    // SemanticTokens
void analysis_semantic_legend(jbuf *out);                                    // SemanticTokensLegend
void analysis_references(const char *uri, int line, int character, bool declaration, jbuf *out); // Location[]
void analysis_highlights(int line, int character, jbuf *out);                // DocumentHighlight[]
void analysis_signature_help(int line, int character, jbuf *out);           // SignatureHelp or null

// These return an error message for the user instead of writing a result
// when they can't do their job, and NULL when they did.
const char *analysis_prepare_rename(int line, int character, jbuf *out);    // Range
const char *analysis_rename(const char *uri, int line, int character, const char *new_name, jbuf *out); // WorkspaceEdit
const char *analysis_format(int tab_size, bool insert_spaces, jbuf *out);   // TextEdit[]
void analysis_code_lenses(jbuf *out);                                        // CodeLens[]
void analysis_code_actions(int start_line, int end_line, jbuf *out);         // CodeAction[]

// Whether the editor can create files in an edit, which moving a declaration
// to a file of its own needs. From its capabilities, at initialization.
void analysis_set_can_create_files(bool can);

// Whether the editor shows a diagnostic's notes about other places there
// (relatedInformation), so they can leave its message. Also from its
// capabilities.
void analysis_set_related_information(bool shows);
