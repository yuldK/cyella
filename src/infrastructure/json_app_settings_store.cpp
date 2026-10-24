#include "infrastructure/json_app_settings_store.h"

#include "domain/path_syntax.h"
#include "domain/repository_path.h"
#include "domain/text_catalog.h"
#include "infrastructure/json_command_settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cyella {
    namespace {
        using json = nlohmann::ordered_json;

        std::string_view as_bytes(const std::u8string_view value) noexcept
        {
            return {
                reinterpret_cast<const char*>(value.data()),
                value.size(),
            };
        }

        std::string as_string(const std::u8string_view value)
        {
            return std::string { as_bytes(value) };
        }

        std::u8string as_u8string(const std::string_view value)
        {
            return std::u8string {
                reinterpret_cast<const char8_t*>(value.data()),
                value.size(),
            };
        }

        diagnostic make_diagnostic(const diagnostic_code code, const diagnostic_severity severity, std::u8string message, const std::optional<std::uint32_t> native_error = std::nullopt)
        {
            diagnostic value {};
            value.code = code;
            value.severity = severity;
            value.message = std::move(message);
            value.native_error = native_error;
            return value;
        }

        // 필드 하나가 틀린 것은 시작을 막지 않는다. 경고를 남기고 그 필드만 기본값을 쓴다.
        void warn(app_settings_load_result& result, std::u8string message)
        {
            result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_invalid, diagnostic_severity::warning, std::move(message)));
        }

        // LF를 CRLF로 바꾸고 마지막 줄바꿈을 보장한다. 저장소의 파일 규칙과 같다.
        std::u8string format_json_bytes(const std::string& dumped)
        {
            std::u8string result {};
            result.reserve(dumped.size() + (dumped.size() / 16) + 2);
            for (const char value : dumped)
            {
                if (value == '\n')
                    result.push_back(u8'\r');
                result.push_back(static_cast<char8_t>(value));
            }
            result.append(u8"\r\n");
            return result;
        }

        json shadow_root(const std::u8string_view shadow_source_json)
        {
            if (shadow_source_json.empty())
                return json::object();

            const std::string_view source_bytes { as_bytes(shadow_source_json) };
            json root { json::parse(source_bytes.begin(), source_bytes.end(), nullptr, false) };
            if (root.is_discarded() || root.is_object() == false)
                return json::object();
            return root;
        }

        // 기존 object 위에 덮어쓰기 위해 꺼낸다. 없거나 object가 아니면 빈 object다.
        json existing_object(const json& root, const char* const key)
        {
            const auto found { root.find(key) };
            if (found != root.end() && found->is_object())
                return *found;
            return json::object();
        }

        std::u8string read_string(const json& value, const char* const key)
        {
            const auto found { value.find(key) };
            if (found == value.end() || found->is_string() == false)
                return {};
            return as_u8string(found->get_ref<const std::string&>());
        }

        bool is_blank(const std::u8string_view text) noexcept
        {
            return std::ranges::all_of(text, [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; });
        }

        // 표시 상태라 어떤 오류도 앱 시작을 막지 않는다. 읽을 수 없으면 경고만 남기고 배치
        // 없이 연다.
        std::optional<window_placement> parse_window(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("window") };
            if (source == root.end() || source->is_null())
                return std::nullopt;
            if (source->is_object() == false)
            {
                warn(result, tr(u8"app_settings.window_not_object"));
                return std::nullopt;
            }

            constexpr std::array coordinate_fields {
                std::string_view { "x" },
                std::string_view { "y" },
                std::string_view { "width" },
                std::string_view { "height" },
            };
            std::array<std::int32_t, 4> coordinates {};
            for (std::size_t index = 0; index < coordinate_fields.size(); ++index)
            {
                const auto value { source->find(coordinate_fields[index]) };
                if (value == source->end() || value->is_number_integer() == false)
                {
                    warn(result, tr(u8"app_settings.window_not_integer"));
                    return std::nullopt;
                }
                const std::int64_t number { value->get<std::int64_t>() };
                if (number < std::numeric_limits<std::int32_t>::min() || number > std::numeric_limits<std::int32_t>::max())
                {
                    warn(result, tr(u8"app_settings.window_out_of_range"));
                    return std::nullopt;
                }
                coordinates[index] = static_cast<std::int32_t>(number);
            }

            window_placement placement {};
            placement.x = coordinates[0];
            placement.y = coordinates[1];
            placement.width = coordinates[2];
            placement.height = coordinates[3];
            if (placement.valid() == false)
            {
                warn(result, tr(u8"app_settings.window_not_positive"));
                return std::nullopt;
            }

            if (const auto maximized { source->find("maximized") }; maximized != source->end() && maximized->is_boolean())
                placement.maximized = maximized->get<bool>();
            return placement;
        }

        void parse_schema_version(const json& root, app_settings_load_result& result)
        {
            const auto version { root.find("schema_version") };
            if (version == root.end() || version->is_null())
                return;
            if (version->is_number_integer() == false)
            {
                warn(result, tr(u8"app_settings.schema_version_not_integer"));
                return;
            }

            const std::int64_t number { version->get<std::int64_t>() };
            if (number > current_app_settings_schema_version)
            {
                // 더 새 앱이 쓴 파일이다. 아는 필드만 읽고, 저장해도 버전 번호를 낮추지 않는다.
                // 낮추면 새 앱이 이 파일을 옛 형식으로 오해해 되돌려 이전하려 들 수 있다.
                result.settings.schema_version = static_cast<std::int32_t>(std::min<std::int64_t>(number, std::numeric_limits<std::int32_t>::max()));
                warn(result, tr(u8"app_settings.newer_schema_version"));
            }
            // 1 미만은 버전이 없던 시절의 값이라 현재 버전으로 본다.
        }

        void parse_git_executable(const json& root, app_settings_load_result& result)
        {
            const auto value { root.find("git_executable") };
            if (value == root.end() || value->is_null())
                return;
            if (value->is_string() == false)
            {
                warn(result, tr(u8"app_settings.git_executable_not_string"));
                return;
            }

            std::u8string executable { as_u8string(value->get_ref<const std::string&>()) };
            // 빈 값은 "지정하지 않음"이며 자동 탐색으로 간다.
            if (executable.empty() == false && is_absolute_windows_path(executable) == false)
            {
                warn(result, tr(u8"app_settings.git_executable_not_absolute"));
                return;
            }
            result.settings.git_executable = std::move(executable);
        }

        void parse_external_diff_tools(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("external_diff_tools") };
            if (source == root.end() || source->is_null())
                return;
            if (source->is_array() == false)
            {
                warn(result, tr(u8"app_settings.external_diff_tools_not_array"));
                return;
            }

            std::size_t dropped { 0 };
            for (const json& entry : *source)
            {
                external_diff_tool tool {};
                if (entry.is_object())
                {
                    tool.executable = read_string(entry, "executable");
                    tool.arguments = read_string(entry, "arguments");
                    if (const auto extensions { entry.find("extensions") }; extensions != entry.end() && extensions->is_array())
                        for (const json& extension : *extensions)
                            if (extension.is_string())
                                if (std::u8string normalized { normalize_diff_extension(as_u8string(extension.get_ref<const std::string&>())) }; normalized.empty() == false)
                                    tool.extensions.push_back(std::move(normalized));
                }

                // 확장자나 실행 파일이 없는 줄은 어떤 파일에도 쓸 수 없다.
                if (tool.extensions.empty() || tool.executable.empty())
                {
                    ++dropped;
                    continue;
                }
                result.settings.external_diff_tools.push_back(std::move(tool));
            }

            if (dropped > 0)
                warn(result, tr(u8"app_settings.external_diff_tool_incomplete"));
        }

        void parse_external_file_apps(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("external_file_apps") };
            if (source == root.end() || source->is_null())
                return;
            if (source->is_array() == false)
            {
                warn(result, tr(u8"app_settings.external_file_apps_not_array"));
                return;
            }
            result.settings.external_file_apps.clear();
            for (const json& entry : *source)
            {
                if (entry.is_object() == false)
                {
                    warn(result, tr(u8"app_settings.file_app_not_object"));
                    continue;
                }
                external_file_app app { read_string(entry, "name"), read_string(entry, "executable"), read_string(entry, "arguments") };
                if (is_blank(app.name) || is_blank(app.executable))
                {
                    warn(result, tr(u8"app_settings.file_app_incomplete"));
                    continue;
                }
                result.settings.external_file_apps.push_back(std::move(app));
            }
        }

        void parse_appearance(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("appearance") };
            if (source == root.end() || source->is_null())
                return;
            if (source->is_object() == false)
            {
                warn(result, tr(u8"app_settings.appearance_not_object"));
                return;
            }

            if (const auto gravatar { source->find("gravatar") }; gravatar != source->end() && gravatar->is_null() == false)
            {
                if (gravatar->is_boolean())
                    result.settings.appearance.gravatar = gravatar->get<bool>();
                else
                    warn(result, tr(u8"app_settings.gravatar_not_bool"));
            }

            if (const auto accent { source->find("accent") }; accent != source->end() && accent->is_null() == false)
            {
                if (accent->is_string())
                    result.settings.appearance.accent = as_u8string(accent->get_ref<const std::string&>());
                else
                    warn(result, tr(u8"app_settings.accent_not_string"));
            }

            if (const auto tabs { source->find("tabs") }; tabs != source->end() && tabs->is_null() == false)
            {
                std::optional<tab_shape> parsed {};
                if (tabs->is_string())
                    parsed = parse_tab_shape(as_u8string(tabs->get_ref<const std::string&>()));
                if (parsed.has_value())
                    result.settings.appearance.tabs = *parsed;
                else
                    warn(result, tr(u8"app_settings.tabs_invalid"));
            }

            if (const auto marker { source->find("tab_marker") }; marker != source->end() && marker->is_null() == false)
            {
                std::optional<tab_marker_place> parsed {};
                if (marker->is_string())
                    parsed = parse_tab_marker_place(as_u8string(marker->get_ref<const std::string&>()));
                if (parsed.has_value())
                    result.settings.appearance.tab_marker = *parsed;
                else
                    warn(result, tr(u8"app_settings.tab_marker_invalid"));
            }

            for (const char* const key : { "ui_font", "code_font" })
            {
                const auto field { source->find(key) };
                if (field == source->end() || field->is_null())
                    continue;
                if (field->is_string())
                    (std::string_view { key } == "ui_font" ? result.settings.appearance.ui_font : result.settings.appearance.code_font) = as_u8string(field->get_ref<const std::string&>());
                else
                    warn(result, tr(u8"app_settings.font_not_string"));
            }

            const auto theme { source->find("theme") };
            if (theme == source->end() || theme->is_null())
                return;

            std::optional<theme_preference> parsed {};
            if (theme->is_string())
                parsed = parse_theme_preference(as_u8string(theme->get_ref<const std::string&>()));
            if (parsed.has_value() == false)
            {
                warn(result, tr(u8"app_settings.theme_invalid"));
                return;
            }
            result.settings.appearance.theme = *parsed;
        }

        void parse_language(const json& root, app_settings_load_result& result)
        {
            const auto language { root.find("language") };
            if (language == root.end() || language->is_null())
                return;

            std::optional<language_preference> parsed {};
            if (language->is_string())
                parsed = parse_language_preference(as_u8string(language->get_ref<const std::string&>()));
            if (parsed.has_value() == false)
            {
                warn(result, tr(u8"app_settings.language_invalid"));
                return;
            }
            result.settings.language = *parsed;
        }

        void parse_recent_repositories(const json& root, app_settings_load_result& result)
        {
            const auto recent { root.find("recent_repositories") };
            if (recent == root.end() || recent->is_null())
                return;
            if (recent->is_array() == false)
            {
                warn(result, tr(u8"app_settings.recent_repositories_not_array"));
                return;
            }

            for (const json& value : *recent)
            {
                if (value.is_object() == false)
                    continue;

                recent_repository entry {};
                entry.path = normalize_repository_path(read_string(value, "path"));
                if (entry.path.empty())
                    continue;
                entry.display_name = read_string(value, "display_name");
                if (entry.display_name.empty())
                    entry.display_name = repository_display_name(entry.path);
                entry.opened_at = read_string(value, "opened_at");
                const auto pinned { value.find("pinned") };
                entry.pinned = pinned != value.end() && pinned->is_boolean() && pinned->get<bool>();
                result.settings.recent_repositories.push_back(std::move(entry));
            }
            // 상한은 모두 읽은 뒤에 적용한다. 앞에서 끊으면 뒤에 있는 고정 항목을 잃는다.
            normalize_recent_repositories(result.settings.recent_repositories);
        }

        // 판 폭 하나다. 양수이고 화면 폭으로 쓸 수 있는 크기여야 한다. 아니면 경고를 남기고 기본 폭을 쓴다.
        std::optional<float> parse_pane_width(const json& panes, const char* const key, app_settings_load_result& result)
        {
            const auto found { panes.find(key) };
            if (found == panes.end() || found->is_null())
                return std::nullopt;
            constexpr double maximum_pane_width { 8192.0 };
            if (found->is_number() == false || found->get<double>() <= 0.0 || found->get<double>() > maximum_pane_width)
            {
                warn(result, tr(u8"app_settings.pane_width_not_positive"));
                return std::nullopt;
            }
            return static_cast<float>(found->get<double>());
        }

        void parse_ui(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("ui") };
            if (source == root.end() || source->is_null())
                return;
            if (source->is_object() == false)
            {
                warn(result, tr(u8"app_settings.ui_not_object"));
                return;
            }
            if (const auto view { source->find("commit_files_view") }; view != source->end() && view->is_null() == false)
            {
                if (view->is_string() && view->get<std::string>() == "tree")
                    result.settings.ui.commit_files_tree = true;
                else if (view->is_string() == false || view->get<std::string>() != "list")
                    warn(result, tr(u8"app_settings.commit_files_view_invalid"));
            }
            if (const auto view { source->find("working_files_view") }; view != source->end() && view->is_null() == false)
            {
                if (view->is_string() && view->get<std::string>() == "tree")
                    result.settings.ui.working_files_tree = true;
                else if (view->is_string() == false || view->get<std::string>() != "list")
                    warn(result, tr(u8"app_settings.working_files_view_invalid"));
            }
            if (const auto layout { source->find("repository_layout") }; layout != source->end() && layout->is_null() == false)
            {
                if (layout->is_string() && layout->get<std::string>() == "review")
                    result.settings.ui.graph_layout = false;
                else if (layout->is_string() == false || layout->get<std::string>() != "graph")
                    warn(result, tr(u8"app_settings.repository_layout_invalid"));
            }
            const auto panes { source->find("pane_widths") };
            if (panes == source->end() || panes->is_null())
                return;
            if (panes->is_object() == false)
            {
                warn(result, tr(u8"app_settings.pane_widths_not_object"));
                return;
            }
            result.settings.ui.pane_widths.sidebar = parse_pane_width(*panes, "sidebar", result);
            result.settings.ui.pane_widths.detail = parse_pane_width(*panes, "detail", result);
        }

        // `diff` 객체다. 보기(`layout`: inline·split)와 문맥 줄 수(`context_lines`: 0~25의 정수). 틀린 필드만 기본값으로 둔다.
        void parse_diff(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("diff") };
            if (source == root.end() || source->is_null())
                return;
            if (source->is_object() == false)
            {
                warn(result, tr(u8"app_settings.diff_not_object"));
                return;
            }
            if (const auto layout { source->find("layout") }; layout != source->end() && layout->is_null() == false)
            {
                std::optional<diff_layout_preference> parsed {};
                if (layout->is_string())
                    parsed = parse_diff_layout_preference(as_u8string(layout->get_ref<const std::string&>()));
                if (parsed.has_value())
                    result.settings.diff.layout = *parsed;
                else
                    warn(result, tr(u8"app_settings.diff_layout_invalid"));
            }
            if (const auto context { source->find("context_lines") }; context != source->end() && context->is_null() == false)
            {
                if (context->is_number_unsigned() && context->get<std::uint64_t>() <= maximum_diff_context_lines)
                    result.settings.diff.context_lines = static_cast<std::uint32_t>(context->get<std::uint64_t>());
                else
                    warn(result, tr(u8"app_settings.diff_context_lines_invalid"));
            }
        }

        // `hosting.hosts[]`: 자체 호스팅 서버 표다. 틀린 줄만 건너뛴다.
        void parse_hosting(const json& root, app_settings_load_result& result)
        {
            const auto source { root.find("hosting") };
            if (source == root.end() || source->is_null())
                return;
            if (source->is_object() == false)
            {
                warn(result, tr(u8"app_settings.hosting_not_object"));
                return;
            }
            const auto hosts { source->find("hosts") };
            if (hosts == source->end() || hosts->is_null())
                return;
            if (hosts->is_array() == false)
            {
                warn(result, tr(u8"app_settings.hosting_hosts_not_array"));
                return;
            }
            bool skipped { false };
            for (const json& entry : *hosts)
            {
                if (entry.is_object() == false)
                {
                    skipped = true;
                    continue;
                }
                const auto host { entry.find("host") };
                const auto provider { entry.find("provider") };
                const auto api_base { entry.find("api_base") };
                if (host == entry.end() || host->is_string() == false || provider == entry.end() || provider->is_string() == false)
                {
                    skipped = true;
                    continue;
                }
                hosting_host_setting setting {};
                setting.host = normalize_hosting_host(host->get_ref<const std::string&>());
                const std::optional<hosting_provider> parsed { parse_hosting_provider(provider->get_ref<const std::string&>()) };
                if (parsed.has_value() == false)
                {
                    skipped = true;
                    continue;
                }
                setting.provider = *parsed;
                if (api_base != entry.end() && api_base->is_string())
                    setting.api_base = api_base->get<std::string>();
                if (validate_hosting_host_setting(setting).has_value()
                    || std::ranges::any_of(result.settings.hosting.hosts, [&setting](const hosting_host_setting& other) { return other.host == setting.host; }))
                {
                    skipped = true;
                    continue;
                }
                result.settings.hosting.hosts.push_back(std::move(setting));
            }
            if (skipped)
                warn(result, tr(u8"app_settings.hosting_hosts_invalid"));
        }

        void parse_open_tabs(const json& root, app_settings_load_result& result)
        {
            const auto tabs { root.find("open_tabs") };
            if (tabs != root.end() && tabs->is_null() == false)
            {
                if (tabs->is_array() == false)
                    warn(result, tr(u8"app_settings.open_tabs_not_array"));
                else
                    for (const json& value : *tabs)
                        if (value.is_string() && value.get_ref<const std::string&>().empty() == false)
                            result.settings.open_tabs.push_back(normalize_repository_path(as_u8string(value.get_ref<const std::string&>())));
            }

            const auto active { root.find("active_tab") };
            if (active != root.end() && active->is_string())
                result.settings.active_tab = normalize_repository_path(as_u8string(active->get_ref<const std::string&>()));
        }
    } // namespace

    std::u8string format_backup_timestamp(const std::chrono::system_clock::time_point time)
    {
        std::u8string text { format_utc_timestamp(time) };
        std::erase_if(text, [](const char8_t value) { return value == u8'-' || value == u8':'; });
        return text;
    }

    std::u8string serialize_app_settings_json(const app_settings& settings, const std::u8string_view shadow_source_json)
    {
        json root { shadow_root(shadow_source_json) };

        // 더 새 앱이 쓴 버전 번호는 낮추지 않는다.
        root["schema_version"] = std::max(settings.schema_version, current_app_settings_schema_version);

        json recent { json::array() };
        for (const recent_repository& value : settings.recent_repositories)
        {
            json entry { json::object() };
            entry["path"] = as_string(value.path);
            entry["display_name"] = as_string(value.display_name);
            entry["opened_at"] = as_string(value.opened_at);
            // 참일 때만 쓴다. 고정을 쓰지 않는 사용자의 파일을 짧게 두려는 것이다.
            if (value.pinned)
                entry["pinned"] = true;
            recent.push_back(std::move(entry));
        }
        root["recent_repositories"] = std::move(recent);

        json tabs { json::array() };
        for (const std::u8string& path : settings.open_tabs)
            tabs.push_back(as_string(path));
        root["open_tabs"] = std::move(tabs);
        root["active_tab"] = as_string(settings.active_tab);

        root["git_executable"] = as_string(settings.git_executable);

        json tools { json::array() };
        for (const external_diff_tool& tool : settings.external_diff_tools)
        {
            json entry { json::object() };
            json extensions { json::array() };
            for (const std::u8string& extension : tool.extensions)
                extensions.push_back(as_string(extension));
            entry["extensions"] = std::move(extensions);
            entry["executable"] = as_string(tool.executable);
            entry["arguments"] = as_string(tool.arguments);
            tools.push_back(std::move(entry));
        }
        root["external_diff_tools"] = std::move(tools);
        json file_apps { json::array() };
        for (const external_file_app& app : settings.external_file_apps)
            file_apps.push_back({ { "name", as_string(app.name) }, { "executable", as_string(app.executable) }, { "arguments", as_string(app.arguments) } });
        root["external_file_apps"] = std::move(file_apps);
        root["language"] = as_string(language_preference_name(settings.language));

        // 객체 안의 알 수 없는 키도 살아남도록 기존 object 위에 덮어쓴다.
        {
            json appearance { existing_object(root, "appearance") };
            appearance["theme"] = as_string(theme_preference_name(settings.appearance.theme));
            appearance["gravatar"] = settings.appearance.gravatar;
            appearance["accent"] = as_string(settings.appearance.accent);
            appearance["tabs"] = as_string(tab_shape_name(settings.appearance.tabs));
            appearance["tab_marker"] = as_string(tab_marker_place_name(settings.appearance.tab_marker));
            appearance["ui_font"] = as_string(settings.appearance.ui_font);
            appearance["code_font"] = as_string(settings.appearance.code_font);
            root["appearance"] = std::move(appearance);
        }

        {
            json diff { existing_object(root, "diff") };
            diff["layout"] = as_string(diff_layout_preference_name(settings.diff.layout));
            diff["context_lines"] = settings.diff.context_lines;
            root["diff"] = std::move(diff);
        }

        // ui 객체는 기본값과 다른 값이 있을 때만 쓴다. 쓰지 않는 사용자의 파일을 짧게 두고, 알 수 없는 키는 보존한다.
        const bool has_widths { settings.ui.pane_widths.sidebar.has_value() || settings.ui.pane_widths.detail.has_value() };
        // 보기는 트리를 골랐거나, 골랐다가 목록으로 되돌린 파일(키가 이미 있다)에만 쓴다.
        const bool has_view { settings.ui.commit_files_tree || existing_object(root, "ui").contains("commit_files_view") };
        const bool has_working_view { settings.ui.working_files_tree || existing_object(root, "ui").contains("working_files_view") };
        const bool has_layout { settings.ui.graph_layout == false || existing_object(root, "ui").contains("repository_layout") };
        if (has_widths || has_view || has_working_view || has_layout)
        {
            json ui { existing_object(root, "ui") };
            if (has_widths)
            {
                json panes { existing_object(ui, "pane_widths") };
                const auto write_width = [&panes](const char* const key, const std::optional<float>& width) {
                    // 끌어서 얻은 float(237.33334350585938)를 그대로 쓰면 파일이 지저분하다. 0.01 논리 픽셀로 맞춘다.
                    if (width.has_value())
                        panes[key] = std::round(static_cast<double>(*width) * 100.0) / 100.0;
                    else
                        panes.erase(key);
                };
                write_width("sidebar", settings.ui.pane_widths.sidebar);
                write_width("detail", settings.ui.pane_widths.detail);
                ui["pane_widths"] = std::move(panes);
            }
            if (has_view)
                ui["commit_files_view"] = settings.ui.commit_files_tree ? "tree" : "list";
            if (has_working_view)
                ui["working_files_view"] = settings.ui.working_files_tree ? "tree" : "list";
            if (has_layout)
                ui["repository_layout"] = settings.ui.graph_layout ? "graph" : "review";
            root["ui"] = std::move(ui);
        }

        // hosting 객체는 호스트 표가 있을 때만 쓴다. 알 수 없는 키는 보존한다.
        if (settings.hosting.hosts.empty() == false)
        {
            json hosting { existing_object(root, "hosting") };
            json hosts { json::array() };
            for (const hosting_host_setting& host : settings.hosting.hosts)
            {
                json entry { json::object() };
                entry["host"] = host.host;
                entry["provider"] = std::string { hosting_provider_name(host.provider) };
                entry["api_base"] = host.api_base;
                hosts.push_back(std::move(entry));
            }
            hosting["hosts"] = std::move(hosts);
            root["hosting"] = std::move(hosting);
        }
        else if (root.contains("hosting") && root["hosting"].is_object())
        {
            root["hosting"].erase("hosts");
            if (root["hosting"].empty())
                root.erase("hosting");
        }

        if (settings.window.has_value())
        {
            json window { existing_object(root, "window") };
            window["x"] = settings.window->x;
            window["y"] = settings.window->y;
            window["width"] = settings.window->width;
            window["height"] = settings.window->height;
            window["maximized"] = settings.window->maximized;
            root["window"] = std::move(window);
        }
        else
            root.erase("window");

        // 프로필·단축키·명령 기록이다. 새 키는 이 한 줄 뒤에 덧붙인다.
        serialize_command_settings(settings, root);

        // 잘못된 UTF-8이 들어와도 던지지 않고 대체 문자로 쓴다. 저장이 예외로 죽는 것보다 낫다.
        return format_json_bytes(root.dump(4, ' ', false, json::error_handler_t::replace));
    }

    app_settings_load_result parse_app_settings_json(const std::u8string_view source_json)
    {
        app_settings_load_result result {};
        const std::string_view source_bytes { as_bytes(source_json) };
        json root { json::parse(source_bytes.begin(), source_bytes.end(), nullptr, false) };
        if (root.is_discarded() || root.is_object() == false)
        {
            result.corrupt = true;
            warn(result, tr(u8"app_settings.root_not_object"));
            return result;
        }

        // 알 수 없는 키는 저장할 때 그대로 돌려 놓는다. 이후 버전이 추가한 항목을 이전 버전이
        // 지우지 않게 하는 규칙이다.
        result.shadow_source_json = std::u8string { source_json };

        parse_schema_version(root, result);
        parse_recent_repositories(root, result);
        parse_open_tabs(root, result);
        parse_git_executable(root, result);
        parse_external_diff_tools(root, result);
        parse_external_file_apps(root, result);
        parse_appearance(root, result);
        result.settings.window = parse_window(root, result);
        parse_ui(root, result);
        parse_diff(root, result);
        parse_hosting(root, result);
        parse_command_settings(root, result);
        parse_language(root, result);
        return result;
    }

    json_app_settings_store::json_app_settings_store(settings_file_system& file_system, clock_function clock) noexcept
        : file_system_ { &file_system }
        , clock_ { std::move(clock) }
    {
        if (clock_ == nullptr)
            clock_ = [] { return std::chrono::system_clock::now(); };
    }

    app_settings_load_result json_app_settings_store::load(const std::u8string_view path) noexcept
    {
        try
        {
            const settings_file_read_result source { file_system_->read(path) };
            if (source.state == settings_file_read_state::not_found)
                return {};
            if (source.state == settings_file_read_state::failed)
            {
                app_settings_load_result result {};
                result.read_failed = true;
                result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_read_failed, diagnostic_severity::warning, tr(u8"app_settings.read_failed"), source.native_error));
                return result;
            }
            // 빈 파일은 깨진 것이 아니라 아직 아무것도 쓰지 않은 것이다. 백업할 내용이 없다.
            if (is_blank(source.bytes))
                return {};

            app_settings_load_result result { parse_app_settings_json(source.bytes) };
            if (result.corrupt == false)
                return result;

            // 깨진 파일은 지우지 않고 옮겨 둔다. 사용자가 손으로 고쳤다면 되살릴 수 있다.
            // 같은 초에 두 번 깨져도 이전 백업을 덮어쓰지 않도록 번호를 붙여 본다.
            const std::u8string stamp { format_backup_timestamp(clock_()) };
            std::optional<std::u8string> backup_path {};
            std::optional<std::uint32_t> backup_error {};
            for (std::size_t attempt = 0; attempt < 10 && backup_path.has_value() == false; ++attempt)
            {
                std::u8string candidate { path };
                candidate.append(u8".bad-");
                candidate.append(stamp);
                if (attempt > 0)
                {
                    candidate.push_back(u8'-');
                    candidate.append(as_u8string(std::to_string(attempt)));
                }

                const settings_file_rename_result renamed { file_system_->rename(path, candidate) };
                if (renamed.succeeded)
                    backup_path = std::move(candidate);
                else
                    backup_error = renamed.native_error;
            }

            result.diagnostics.clear();
            if (backup_path.has_value())
            {
                std::u8string message { tr(u8"app_settings.corrupt_backed_up", { { u8"path", *backup_path } }) };
                result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_invalid, diagnostic_severity::warning, std::move(message)));
            }
            else
                result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_backup_failed, diagnostic_severity::warning, tr(u8"app_settings.corrupt_backup_failed"), backup_error));
            return result;
        }
        catch (...)
        {
            // 파일 상태를 모르는 채 끝났으므로 읽기 실패와 같게 다룬다.
            app_settings_load_result result {};
            result.read_failed = true;
            try
            {
                result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_read_failed, diagnostic_severity::warning, tr(u8"app_settings.read_unexpected_error")));
            }
            catch (...)
            {}
            return result;
        }
    }

    app_settings_save_result json_app_settings_store::save(const std::u8string_view path, const app_settings& settings, const std::u8string_view shadow_source_json) noexcept
    {
        try
        {
            std::u8string bytes { serialize_app_settings_json(settings, shadow_source_json) };
            const settings_file_commit_result commit { file_system_->atomic_commit(path, bytes) };

            app_settings_save_result result {};
            if (commit.succeeded() == false)
            {
                result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_write_failed, diagnostic_severity::warning, tr(u8"app_settings.write_failed"), commit.native_error));
                return result;
            }

            result.succeeded = true;
            result.shadow_source_json = std::move(bytes);
            return result;
        }
        catch (...)
        {
            app_settings_save_result result {};
            try
            {
                result.diagnostics.push_back(make_diagnostic(diagnostic_code::app_settings_write_failed, diagnostic_severity::warning, tr(u8"app_settings.write_unexpected_error")));
            }
            catch (...)
            {}
            return result;
        }
    }
} // namespace cyella
