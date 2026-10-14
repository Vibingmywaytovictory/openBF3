#pragma once
// bf3dump text commands: the UITextDatabase (bf3_core ui/text_db.h) and the EBX SID census
// (ui/text_census.h).

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

// text <sid-or-hash>...: one line per argument: input, hash, text (escaped) or "absent -> <fallback>".
int cmd_text_lookup(const EbxContext &p_ctx, const std::vector<std::string> &p_keys);
// text --census [<sb-spec>]: database header, chunk SHA1s, entry count, tag, decode errors, and how many
// distinct EBX SIDs resolve (the research partition set, or every partition of <sb-spec>).
int cmd_text_census(const EbxContext &p_ctx, const std::string &p_spec);
// text --tsv <out.tsv>: every entry as hash \t text (\\, \t, \n, \r escaped), in hash order.
int cmd_text_tsv(const EbxContext &p_ctx, const std::string &p_out);

} // namespace bf3dump
