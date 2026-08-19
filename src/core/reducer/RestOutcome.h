// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Turns an HTTP status plus the protocol-relevant body fields into a decision the
// session FSM acts on. The single encoding of the contract's error model.

#pragma once

#include <cstdint>
#include <string>

namespace dish::reducer {

enum class RestVerdict {
    Ok,              // 2xx with the fields we need
    Unauthorized,    // 401 NOT_PAIRED|BAD_PROOF; terminal, drop key and re-pair
    VersionMismatch, // 409; terminal, client/server protocol skew
    ShuttingDown,    // 503
    Unreachable,     // transport failure or empty body
    ServerError,     // any other non-2xx with a body
};

inline bool restVerdictTerminal(RestVerdict v) {
    return v == RestVerdict::Unauthorized || v == RestVerdict::VersionMismatch;
}

inline bool restVerdictRetryable(RestVerdict v) {
    return v == RestVerdict::Unreachable || v == RestVerdict::ShuttingDown ||
           v == RestVerdict::ServerError;
}

// A status of 0 is the gateway's sentinel for "the transport never produced a
// response".
struct RestReply {
    int status = 0;
    bool bodyParsed = false;
    std::string code; // NOT_PAIRED | BAD_PROOF on a 401, else empty
};

inline RestVerdict classifyRest(const RestReply& r) {
    if (r.status == 0 || !r.bodyParsed) { return RestVerdict::Unreachable; }
    if (r.status >= 200 && r.status <= 299) { return RestVerdict::Ok; }
    if (r.status == 401) { return RestVerdict::Unauthorized; }
    if (r.status == 409) { return RestVerdict::VersionMismatch; }
    if (r.status == 503) { return RestVerdict::ShuttingDown; }
    return RestVerdict::ServerError;
}

// ── Pairing classification ──────────────────────────────────────────────────
// POST /api/pair always answers 200 on the PIN paths, so the dish classifies on
// the body's ok/pending fields rather than the HTTP status.

enum class PairVerdict {
    Success,         // ok with a sharedKey: adopt it and open the session
    Pending,         // Path B {ok:false, pending:true}: poll /api/pair/status
    AuthRequired,    // reachable but no key: first-time pair, or it forgot us
    VersionMismatch, // 409
    Unreachable,     // transport failure or empty body
};

struct PairReply {
    int status = 0;
    bool bodyParsed = false;
    bool ok = false;
    bool pending = false;
    bool hasSharedKey = false; // sharedKey present AND non-empty
};

inline PairVerdict classifyPair(const PairReply& r) {
    if (r.status == 409) { return PairVerdict::VersionMismatch; }
    if (r.status == 0 || !r.bodyParsed) { return PairVerdict::Unreachable; }
    if (r.ok && r.hasSharedKey) { return PairVerdict::Success; }
    if (r.pending) { return PairVerdict::Pending; }
    // A server answering ok=true with no key lands here too: caching an empty key
    // would break every subsequent reconnect.
    return PairVerdict::AuthRequired;
}

// ── Path-B approval status (GET /api/pair/status) ────────────────────────────
// There is no wire "denied": an operator deny erases the pending row server-side,
// so the client polls straight to "none". That is terminal exactly when a
// "pending" was seen first; before that, "none" tolerates the POST-to-first-poll
// race. "denied" is still mapped for satellites predating the change.
enum class ApprovalVerdict { Approved, Pending, Declined, Unreachable };

struct ApprovalReply {
    int status = 0;
    bool bodyParsed = false;
    std::string statusStr; // "approved"|"pending"|"none" ("denied" legacy)
    bool hasSharedKey = false;
};

// `sawPending` is whether any poll in THIS attempt observed "pending"; the caller
// tracks it so this stays a function.
inline ApprovalVerdict classifyApproval(const ApprovalReply& r, bool sawPending = false) {
    if (r.status == 0 || !r.bodyParsed) { return ApprovalVerdict::Unreachable; }
    if (r.statusStr == "approved" && r.hasSharedKey) { return ApprovalVerdict::Approved; }
    if (r.statusStr == "denied") { return ApprovalVerdict::Declined; }
    // A "none" after a pending means the deny erased the row. Terminal for this
    // attempt; polling on would burn the whole budget.
    if (r.statusStr == "none" && sawPending) { return ApprovalVerdict::Declined; }
    return ApprovalVerdict::Pending;
}

} // namespace dish::reducer
