// SPDX-License-Identifier: LGPL-2.1-or-later
// Photo Booth's PBAssetCollection: the pictures of ~/Pictures/Photo Booth, in the order of its
// Recents.plist (an array: a file name for a photo or a movie, an array of four for a 4-up), the
// newest last. PBAsset names a new file "Photo on <YYYY-MM-dd> at <HH.mm>.jpg" (4-ups "4-up on ...",
// " #n" before the extension for each of the four and for a name already taken).
#pragma once

#include "graphics.h"

#include <string>
#include <vector>

namespace PB {

struct Asset {
    std::vector<std::string> files;   // names in the folder: one, or a 4-up's four
    bool movie = false;               // a PBQTMovieAsset (.mov): listed and counted, not played here
    AK::Image preview;                // 160 pixels at most (kCGImageSourceThumbnailMaxPixelSize)
};

class Library {
public:
    Library();
    const std::string &root() const { return m_root; }
    std::vector<Asset> &assets() { return m_assets; }

    // -[PBAssetCollection appendNewAsset:]: the files written, the asset last, Recents.plist rewritten
    bool addPhoto(const AK::Image &photo);
    bool addFourUp(const std::vector<AK::Image> &photos);
    // -[PBAsset delete]: the files moved to IntermediateTrashDirectory (in Photo Booth's application
    // support folder) until it quits, for Undo; -undoDelete moves them back
    Asset remove(int index);
    void restore(const Asset &asset, int index);
    // -[PBAssetCollection deleteAllAssets]: every one deleted so
    void removeAll();
    // -applicationWillTerminate:: what was deleted to the Trash
    void emptyIntermediateTrash();
    // -[PBImageAsset flip]: the picture mirrored, in its file
    void flip(int index);
    AK::Image fullImage(int index) const;   // a 4-up's four in a square
    std::string path(int index, int file = 0) const;

private:
    void load();
    void save() const;
    void loadPreview(Asset &asset) const;
    std::string newName(const std::string &templ, const std::string &suffix) const;
    std::string m_root;
    std::vector<Asset> m_assets;
};

// A 4-up's pictures two by two, in a picture of one's size
AK::Image fourUp(const std::vector<AK::Image> &pictures, int width, int height);
AK::Image mirrored(const AK::Image &image);
bool saveJpeg(const AK::Image &image, const std::string &path);

} // namespace PB
