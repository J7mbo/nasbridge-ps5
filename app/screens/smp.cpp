// NASBridge - Talking to ShadowMountPlus.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/smp.hpp"

#include "concepts/connection.hpp"
#include "concepts/http.hpp"
#include "concepts/json_scan.hpp"

#include <cstdio>

namespace hui::concepts
{

namespace
{

int g_smp_port = 10101;

// NASBridge itself: a ShadowMountPlus older than 1.7beta6 counts any open app
// as a running game, and refuses changes until it closes.
const char *const kNasBridgeIsAGame =
    "ShadowMountPlus won't do this while NASBridge is open: update it to 1.7beta6-fix2 or newer";

// One API call. On success the reply's JSON is in *json. Fails with a short
// reason when ShadowMountPlus doesn't answer or says no ("status" non-zero).
bool call(const char *route, const std::string &request, std::string *json, std::string *why,
          int *http_status = nullptr, std::string *reason = nullptr)
{
    if (reason != nullptr)
        reason->clear();
    Connection local;
    local.host = "127.0.0.1";
    local.port = g_smp_port;
    const HttpReply reply = http_request(local, "POST", route,
                                         "Content-Type: application/json\r\n", request,
                                         4u * 1024u * 1024u);
    if (http_status != nullptr)
        *http_status = reply.status;
    if (!reply.connected)
    {
        *why = "ShadowMountPlus isn't answering (is it running?)";
        return false;
    }
    *json = reply.body;
    const char *begin = json->data();
    const char *end = begin + json->size();
    const long long status = json_integer(begin, end, "status", -1);
    if (reply.status >= 200 && reply.status < 300 && status == 0)
        return true;
    std::string error = json_text(begin, end, "error");
    const std::string error_reason = json_text(begin, end, "error_reason");
    if (reason != nullptr)
        *reason = error_reason;
    if (error_reason == "game_active")
        error = kNasBridgeIsAGame;
    if (error.empty())
    {
        char text[64];
        std::snprintf(text, sizeof(text), "ShadowMountPlus said no (HTTP %d)", reply.status);
        error = text;
    }
    *why = error;
    return false;
}

} // namespace

int smp_port()
{
    return g_smp_port;
}

void set_smp_port(int port)
{
    g_smp_port = port;
}

bool smp_games_from_json(const std::string &json, std::vector<SmpGame> *games)
{
    games->clear();
    if (json_value(json.data(), json.data() + json.size(), "games") == nullptr)
        return false;
    json_objects(json, "games",
                 [&](const char *from, const char *to)
                 {
                     SmpGame game;
                     game.title_id = json_text(from, to, "title_id");
                     game.title_name = json_text(from, to, "title_name");
                     game.path = json_text(from, to, "path");
                     game.source_type = json_text(from, to, "source_type");
                     game.installed = json_true(from, to, "installed");
                     game.mounted = json_true(from, to, "mounted");
                     game.can_uninstall = json_true(from, to, "can_uninstall");
                     game.can_manage_source = json_true(from, to, "can_manage_source");
                     if (!game.title_id.empty())
                         games->push_back(game);
                 });
    return true;
}

bool smp_job_from_json(const std::string &json, SmpJob *job)
{
    const char *begin = json.data();
    const char *end = begin + json.size();
    if (json_value(begin, end, "state") == nullptr)
        return false;
    job->id = static_cast<int>(json_integer(begin, end, "job_id", 0));
    job->operation = json_text(begin, end, "operation");
    job->state = json_text(begin, end, "state");
    job->active = json_true(begin, end, "active");
    json_number(begin, end, "processed_bytes", &job->processed_bytes);
    json_number(begin, end, "total_bytes", &job->total_bytes);
    job->error = json_text(begin, end, "result_error");
    job->reason = json_text(begin, end, "result_error_reason");
    if (job->reason == "game_active")
        job->error = kNasBridgeIsAGame; // a delete is refused inside its job
    return true;
}

bool smp_games(std::vector<SmpGame> *games, std::string *why)
{
    std::string json;
    if (!call("/api/v1/games", "{\"include_size\":false}", &json, why))
        return false;
    if (!smp_games_from_json(json, games))
    {
        *why = "ShadowMountPlus sent no game list";
        return false;
    }
    return true;
}

bool smp_scan(bool reset_attempts, std::string *why, std::string *deferred)
{
    std::string json;
    const std::string request = reset_attempts ? "{\"reset_attempts\":true}" : "{}";
    if (!call("/api/v1/scan", request, &json, why))
        return false;
    if (deferred != nullptr)
        *deferred = json_text(json.data(), json.data() + json.size(), "scan_deferred_reason");
    return true;
}

bool smp_delete_source(const std::string &title_id, int *job_id, bool *missing, std::string *why,
                       std::string *reason)
{
    std::string json;
    *job_id = 0;
    *missing = false;
    const std::string request = "{\"title_id\":" + json_quote(title_id) + ",\"confirm\":true}";
    if (!call("/api/v1/games/delete", request, &json, why, nullptr, reason))
        return false;
    const char *begin = json.data();
    const char *end = begin + json.size();
    *missing = json_true(begin, end, "source_missing");
    *job_id = static_cast<int>(json_integer(begin, end, "job_id", 0));
    if (!*missing && *job_id <= 0)
    {
        *why = "ShadowMountPlus didn't start deleting";
        return false;
    }
    return true;
}

bool smp_job(int job_id, SmpJob *job, std::string *why)
{
    std::string json;
    if (!call("/api/v1/games/storage/status", "{\"job_id\":" + std::to_string(job_id) + "}", &json,
              why))
        return false;
    if (!smp_job_from_json(json, job))
    {
        *why = "ShadowMountPlus sent no job status";
        return false;
    }
    return true;
}

bool smp_uninstall(const std::string &title_id, std::string *why, std::string *reason)
{
    std::string json;
    const std::string request = "{\"title_id\":" + json_quote(title_id) + "}";
    return call("/api/v1/games/uninstall", request, &json, why, nullptr, reason);
}

std::string smp_reason_text(const std::string &reason)
{
    if (reason == "game_active")
        return "a game is running";
    if (reason == "runtime_prepared")
        return "a game is starting or running";
    if (reason == "scanner_busy" || reason == "runtime_busy")
        return "ShadowMountPlus stayed busy";
    if (reason == "title_unmount_busy")
        return "the PS5 still has the game's files open";
    if (reason == "install_pending")
        return "ShadowMountPlus is still installing something";
    if (reason == "rest_mode")
        return "the PS5 is going into rest mode";
    return "";
}

std::string smp_reason_hint(const std::string &reason)
{
    if (reason == "game_active")
        return "Close that game, then try again.";
    if (reason == "runtime_prepared")
        return "Let the game finish starting and close it, then try again.";
    if (reason == "title_unmount_busy")
        return "Try again in a moment. If it keeps happening, restart the PS5 and try again.";
    if (reason == "install_pending")
        return "Try again once it has finished.";
    if (reason == "rest_mode")
        return "Try again once the PS5 is fully awake.";
    return "Try again in a moment.";
}

} // namespace hui::concepts
