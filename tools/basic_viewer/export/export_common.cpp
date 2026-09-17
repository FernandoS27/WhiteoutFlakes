// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "export_common.h"

#include "io/wem/wem_export.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <exception>
#include <system_error>

namespace whiteout::flakes {

TextureExportCounters& TextureExportCounters::operator+=(const TextureExportCounters& other) {
    exported += other.exported;
    skipped += other.skipped;
    failed += other.failed;
    unused += other.unused;
    inWar3Mod += other.inWar3Mod;
    return *this;
}

std::string WriteModelFile(const std::filesystem::path& path,
                           const std::function<void(const std::string& utf8Path)>& write) {
    std::error_code dirError;
    std::filesystem::create_directories(path.parent_path(), dirError);
    try {
        write(io::PathToUtf8(path));
    } catch (const std::exception& e) {
        return std::string("could not write the model: ") + e.what();
    }
    if (!std::filesystem::exists(path)) {
        return "could not write the model to " + io::PathToUtf8(path);
    }
    return {};
}

std::optional<::whiteout::models::wem::Document> ConvertThroughWem(
    const ExportSubject& subject, const io::WemExportOptions& options, ConversionReport& report) {
    io::WemExportResult exported = io::ExportModelToWem(*subject.source, subject.provider, options);
    report.diagnostics.append(exported.diagnostics);
    if (!exported.ok()) {
        report.error = exported.error;
        return std::nullopt;
    }
    report.formatId = exported.formatId;
    // Moved, not copied: the result is ours, and a document is every mesh and
    // track the model has.
    return std::move(exported.document);
}

} // namespace whiteout::flakes
