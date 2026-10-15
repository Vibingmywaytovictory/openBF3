#pragma once
// bf3dump audio commands (M2a audio; docs/design/m2a-audio.md §1 gates G1-G4, §9): the drivers of the install
// gates and the offline render. Every command prints one machine-readable summary line the ctest scripts parse
// and an install.unchanged line.
//
//   audio waves <LEVEL> <MODE> [--decode] [--bench N]
//       every SoundWaveAsset of the level x mode: read, every playable segment's stream scanned (--decode: decoded)
//       and its frames compared with the header and with SegmentLength x rate; --bench N: N single-thread decodes
//       of all those streams from memory, best thread CPU time (GetThreadTimes) and wall time per channel-second
//   audio loc <lang> [--decode]
//       every chunk of Win32/Loc/<lang> (base + patch toc), every stream in it scanned / decoded
//   audio census <sb-spec>
//       SoundPatchAsset / master / mixer census over the spec (node instances, types, voices, waves, connections),
//       every graph compiled (allow_unimplemented), the handheld weapon patches compiled strictly
//   audio render <LEVEL> <MODE> <patch> --distance <m> [--rounds N] [--seed S] [--first-person] --out <wav>
//       one burst of a weapon patch rendered offline through the mixer core into a float WAV

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

int cmd_audio(const bf3::core::InstallReader &p_reader, std::shared_ptr<bf3::core::Install> p_install, const std::vector<std::string> &p_args);
const char *audio_usage();

} // namespace bf3dump
