// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database's three file shapes read off their JSON with
// QJsonDocument, the same reader core/update/UpdateManifest uses, into the
// plain core/controllerdb records every decision runs on. Nothing here
// decides anything beyond "does this decode": the size cap is checked before
// the parse, a record whose fields do not decode is dropped and counted rather
// than failing its file, and a word this build has not heard of reads as
// Unknown so the record stays, inert (core/controllerdb/DirectModel.h).

#pragma once

#include "core/controllerdb/ControllerRecord.h"

#include <QByteArray>

#include <cstdint>
#include <variant>

namespace dish::source::controllerdb {

enum class DbParseError : std::uint8_t {
    Oversize,          // past the cap, refused unparsed
    BadJson,           // unparseable, or the root is not an object
    UnsupportedSchema, // schema != 1
    BadVersion,        // dbVersion missing or not positive
    BadShape,          // a required member missing or of the wrong type
};

std::variant<dish::controllerdb::ControllerDb, DbParseError>
parseControllerDb(const QByteArray& body);

std::variant<dish::controllerdb::StringsTable, DbParseError>
parseStringsTable(const QByteArray& body);

std::variant<dish::controllerdb::DbManifest, DbParseError> parseDbManifest(const QByteArray& body);

} // namespace dish::source::controllerdb
