#pragma once

// The flags a reconstruction was built with, left in the workspace beside the
// model they produced. Both runners keep a finished model rather than spending
// an hour rebuilding one -- which is how a dataset somebody else reconstructed
// gets masks and geometry -- and this is how they notice that the panel has
// since been asked for a different model.
//
// It only answers that question for the settings that WROTE it: a panel
// pointed at a dataset it did not build is at its defaults, which describe no
// model at all (SfmJob::settings_built_model).

#include <string>
#include <vector>

namespace gui {

// In the workspace root. Not dataset artifacts: no parser looks for them, and
// probe_workspace counts neither as a model. The frames file is the same idea
// one step earlier -- what the pictures in images/ were extracted with.
inline constexpr const char* kReconStampFile = ".spirula-recon";
inline constexpr const char* kFramesStampFile = ".spirula-frames";

struct ReconStamp {
    bool present = false;
    std::string engine;              // "builtin" or "colmap"
    std::vector<std::string> args;   // the reconstruction's own flags, in order
};

ReconStamp read_recon_stamp(const std::string& workspace,
                            const char* file = kReconStampFile);
void write_recon_stamp(const std::string& workspace, const ReconStamp& st,
                       const char* file = kReconStampFile);

// The flag whose value moved since `prior` was written, for the line that says
// why a model is being rebuilt. Empty when nothing moved, and for a workspace
// that carries no stamp.
std::string recon_stamp_change(const ReconStamp& prior, const ReconStamp& now);

}  // namespace gui
