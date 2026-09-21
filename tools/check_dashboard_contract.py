#!/usr/bin/env python3
"""Compile production quota parsing and dashboard state on the host.

No account, network, device or settings are accessed. Inputs include the
1%-becomes-100% regression, malformed schemas, ambiguous pairs and cache aging.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

TEST = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "dashboard/dashboard_data.h"
#include "dashboard/dashboard_json.h"

using namespace dashboard;

void expect_json(const char* input, bool expected, int five = 0, int weekly = 0) {
    cJSON* root = cJSON_Parse(input);
    assert(root);
    int16_t parsed_five = -9, parsed_weekly = -8;
    assert(ParseQuotaRemaining(root, parsed_five, parsed_weekly) == expected);
    if (expected) assert(parsed_five == five && parsed_weekly == weekly);
    else assert(parsed_five == -9 && parsed_weekly == -8);
    cJSON_Delete(root);
}

int main() {
    const double used[] = {0, 1, 98, 99, 99.5, 100};
    const int expected[] = {100, 99, 2, 1, 1, 0};
    for (size_t i = 0; i < 6; ++i) {
        char json[256];
        std::snprintf(json, sizeof(json),
            R"({"rate_limit":{"primary_window":{"used_percent":%.2f},"secondary_window":{"used_percent":%.2f}}})",
            used[i], used[i]);
        expect_json(json, true, expected[i], expected[i]);
    }
    expect_json(R"({"five_hour_remaining":1,"weekly_remaining":0.5})", true, 1, 1);
    expect_json(R"({"five_hour_remaining_ratio":1,"weekly_remaining_ratio":0.5})", true, 100, 50);
    expect_json(R"({"five_hour_remaining_percent":1,"weekly_remaining_percent":0})", true, 1, 0);
    expect_json(R"({"data":{"fiveHourRemaining":27,"weeklyRemaining":55}})", true, 27, 55);
    expect_json(R"({"primary_remaining":21,"secondary_remaining":42})", true, 21, 42);
    expect_json(R"({"primaryRemaining":21,"secondaryRemaining":42})", true, 21, 42);
    expect_json(R"({})", false);
    expect_json(R"({"five_hour_remaining":1})", false);
    expect_json(R"({"five_hour_remaining":"1","weekly_remaining":1})", false);
    expect_json(R"({"five_hour_remaining":null,"weekly_remaining":1})", false);
    expect_json(R"({"five_hour_remaining":-1,"weekly_remaining":1})", false);
    expect_json(R"({"five_hour_remaining":101,"weekly_remaining":1})", false);
    expect_json(R"({"five_hour_remaining":1e100,"weekly_remaining":1})", false);
    expect_json(R"({"five_hour_remaining_ratio":1.1,"weekly_remaining_ratio":1})", false);
    expect_json(R"({"five_hour":1,"weekly":1})", false);
    expect_json(R"({"five_hour_remaining":1,"fiveHourRemaining":1,"weekly_remaining":1})", false);
    expect_json(R"({"five_hour_remaining":1,"five_hour_remaining":1,"weekly_remaining":1})", false);
    expect_json(R"({"a":{"five_hour_remaining":1},"b":{"weekly_remaining":1}})", false);
    expect_json(R"([{"five_hour_remaining":1,"weekly_remaining":1},{"five_hour_remaining":2,"weekly_remaining":2}])", false);
    expect_json(R"({"rate_limit":{"primary_window":{"used_percent":-1},"secondary_window":{"used_percent":1}}})", false);
    expect_json(R"({"rate_limit":{"primary_window":{"used_percent":101},"secondary_window":{"used_percent":1}}})", false);
    expect_json(R"({"rate_limit":{"primary_window":{"used_percent":1}}})", false);
    // cJSON cannot serialize NaN/Inf as a JSON number. Exercise them directly
    // so the production numeric guard is tested rather than a parser fallback.
    const double invalid[] = {std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        -1, 1e300};
    for (double value : invalid) {
        int16_t percent = -1;
        assert(!RemainingPercent(value, QuotaUnit::Percent, false, percent));
        assert(percent == -1);
        assert(!RemainingPercent(value, QuotaUnit::Percent, true, percent));
        assert(!RemainingPercent(value, QuotaUnit::Ratio, false, percent));
        cJSON* root = cJSON_CreateObject();
        cJSON* invalid_value = cJSON_AddNumberToObject(root, "five_hour_remaining", 0);
        invalid_value->valuedouble = value;
        cJSON_AddNumberToObject(root, "weekly_remaining", 1);
        int16_t five = -1, weekly = -1;
        assert(!ParseQuotaRemaining(root, five, weekly));
        assert(five == -1 && weekly == -1);
        cJSON_Delete(root);
    }

    constexpr uint32_t now = 1789700000U;
    assert(EvaluateFreshness(false, now, now, 60) == Freshness::NoData);
    assert(EvaluateFreshness(true, now, 0, 60) == Freshness::UnknownTime);
    assert(EvaluateFreshness(true, 0, now, 60) == Freshness::UnknownTime);
    assert(EvaluateFreshness(true, now + 1, now, 60) == Freshness::UnknownTime);
    assert(EvaluateFreshness(true, now - 59, now, 60) == Freshness::Fresh);
    assert(EvaluateFreshness(true, now - 60, now, 60) == Freshness::Stale);
    auto& data = DashboardData::GetInstance();
    data.ResetDefaults();
    auto snapshot = data.GetSnapshot();
    assert(!snapshot.weather.valid && !snapshot.quota.valid);
    char status[64];
    FormatWeatherStatus(snapshot.weather, status, sizeof(status));
    assert(std::strcmp(status, "无数据") == 0);
    Weather weather;
    weather.valid = true;
    weather.from_cache = true;
    weather.updated_epoch = now;
    data.SetWeather(weather);
    snapshot = data.GetSnapshot();
    assert(snapshot.weather.freshness == Freshness::UnknownTime);
    FormatWeatherStatus(snapshot.weather, status, sizeof(status));
    assert(std::strcmp(status, "缓存·时间未知") == 0);
    data.UpdateFreshness(now);
    snapshot = data.GetSnapshot();
    assert(snapshot.weather.freshness == Freshness::Fresh);
    uint32_t revision = snapshot.revision;
    data.UpdateFreshness(now + 1);
    assert(data.Revision() == revision);  // No redraw on every clock tick.
    data.SetWeatherRequestState(RequestState::Offline);
    snapshot = data.GetSnapshot();
    assert(snapshot.weather.valid && snapshot.weather.updated_epoch == now);
    FormatWeatherStatus(snapshot.weather, status, sizeof(status));
    assert(std::strcmp(status, "离线·缓存") == 0);
    data.UpdateFreshness(now + kWeatherFreshSeconds);
    snapshot = data.GetSnapshot();
    assert(snapshot.weather.freshness == Freshness::Stale);
    FormatWeatherStatus(snapshot.weather, status, sizeof(status));
    assert(std::strcmp(status, "离线·已过期") == 0);
    data.SetWeatherRequestState(RequestState::Refreshing);
    data.SetWeatherRequestState(RequestState::Failed);
    snapshot = data.GetSnapshot();
    assert(snapshot.weather.valid && snapshot.weather.from_cache);
    FormatWeatherStatus(snapshot.weather, status, sizeof(status));
    assert(std::strcmp(status, "更新失败·已过期") == 0);
    weather.from_cache = false;
    weather.updated_epoch = now + kWeatherFreshSeconds;
    weather.request_state = RequestState::Succeeded;
    data.SetWeather(weather);
    snapshot = data.GetSnapshot();
    assert(snapshot.weather.freshness == Freshness::Fresh && !snapshot.weather.from_cache);
    FormatWeatherStatus(snapshot.weather, status, sizeof(status));
    assert(std::strcmp(status, "已更新") == 0);
    data.SetQuotaRequestState(RequestState::Disabled);
    FormatQuotaStatus(data.GetSnapshot().quota, status, sizeof(status));
    assert(std::strcmp(status, "未配置·无数据") == 0);
    Quota quota;
    quota.valid = true;
    quota.updated_epoch = now;
    quota.five_hour_remaining = 1;
    quota.weekly_remaining = 50;
    quota.request_state = RequestState::Succeeded;
    data.UpdateFreshness(now);
    data.SetQuota(quota);
    assert(data.GetSnapshot().quota.freshness == Freshness::Fresh);
    data.UpdateFreshness(now + kQuotaFreshSeconds);
    snapshot = data.GetSnapshot();
    assert(snapshot.quota.freshness == Freshness::Stale);
    assert(snapshot.quota.five_hour_remaining == 1);
    data.SetQuotaRequestState(RequestState::Failed);
    assert(data.GetSnapshot().quota.valid);
    data.UpdateFreshness(0);
    assert(data.GetSnapshot().quota.freshness == Freshness::UnknownTime);
    std::puts("Dashboard contract passed: quota units, bounds, schemas, freshness and request states");
}
'''


def main():
    compiler = shutil.which('c++')
    c_compiler = shutil.which('cc')
    if not compiler or not c_compiler:
        raise SystemExit('Host C and C++ compilers are required')
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    with tempfile.TemporaryDirectory(prefix='miaoink-dashboard-test-') as directory:
        tmp = Path(directory)
        (tmp / 'test.cc').write_text(TEST)
        sanitizers = ['-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all']
        subprocess.run([c_compiler, '-O1', *sanitizers, '-c', str(cjson / 'cJSON.c'),
                        '-o', str(tmp / 'cjson.o')], check=True)
        subprocess.run([compiler, '-std=c++17', '-O1', '-Wall', '-Wextra', *sanitizers,
                        '-I', str(ROOT / 'main'), '-I', str(cjson),
                        str(tmp / 'test.cc'), str(ROOT / 'main/dashboard/dashboard_data.cc'),
                        str(tmp / 'cjson.o'), '-o', str(tmp / 'test')], check=True)
        subprocess.run([str(tmp / 'test')], check=True)


if __name__ == '__main__':
    main()
