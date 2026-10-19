#include "infrastructure/git_ref_parser.h"

#include "infrastructure/git_text.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::size_t ref_field_count { 8 };

        // `[ahead 1, behind 2]`에서 단어 뒤의 숫자를 읽는다. 단어가 없으면 0이다.
        std::uint32_t read_track_count(const std::string_view track, const std::string_view word) noexcept
        {
            const std::size_t position { track.find(word) };
            if (position == std::string_view::npos)
                return 0;

            std::size_t begin { position + word.size() };
            std::size_t end { begin };
            while (end < track.size() && track[end] >= '0' && track[end] <= '9')
                ++end;
            return git_text::parse_integer<std::uint32_t>(track.substr(begin, end - begin)).value_or(0);
        }

        // `refs/remotes/origin/HEAD`면 remote 이름(`origin`)을 돌려준다.
        std::string_view remote_name_of_head(const std::string_view refname) noexcept
        {
            constexpr std::string_view prefix { "refs/remotes/" };
            constexpr std::string_view suffix { "/HEAD" };
            if (git_text::starts_with(refname, prefix) == false || git_text::ends_with(refname, suffix) == false)
                return {};
            if (refname.size() <= prefix.size() + suffix.size())
                return {};
            return refname.substr(prefix.size(), refname.size() - prefix.size() - suffix.size());
        }

        struct parsed_record
        {
            std::array<std::string_view, ref_field_count> fields {};
        };

        bool split_record(std::string_view record, parsed_record& parsed) noexcept
        {
            for (std::size_t index { 0 }; index < ref_field_count; ++index)
            {
                std::string_view field {};
                const bool has_more { git_text::take_field(record, '\x1F', field) };
                parsed.fields[index] = field;
                // 마지막 필드는 구분자 없이 끝난다. 그 앞에서 끝났으면 필드가 모자란다.
                if (has_more == false && index + 1 < ref_field_count)
                    return false;
            }
            return true;
        }
    } // namespace

    git_ref_list parse_git_refs(const std::string_view bytes)
    {
        git_ref_list result {};
        std::vector<std::string_view> upstream_fulls {};
        std::string_view rest { bytes };

        while (rest.empty() == false)
        {
            // 레코드 끝은 줄바꿈이다. `%00`을 덧붙인 형식(NUL 뒤에 줄바꿈)도 받도록
            // 둘 다 구분자로 보고 빈 조각은 건너뛴다.
            const std::size_t end { rest.find_first_of(std::string_view { "\n\0", 2 }) };
            const std::string_view record { rest.substr(0, end) };
            rest = end == std::string_view::npos ? std::string_view {} : rest.substr(end + 1);
            if (record.empty())
                continue;

            parsed_record parsed {};
            if (split_record(record, parsed) == false)
            {
                ++result.skipped_count;
                continue;
            }

            const std::string_view refname { parsed.fields[0] };
            const std::string_view object_name { parsed.fields[1] };
            const std::string_view peeled_name { parsed.fields[2] };
            const std::string_view head_marker { parsed.fields[3] };
            const std::string_view upstream_full { parsed.fields[4] };
            const std::string_view upstream_short { parsed.fields[5] };
            const std::string_view track { parsed.fields[6] };
            const std::string_view symref { parsed.fields[7] };

            if (const std::string_view remote { remote_name_of_head(refname) }; remote.empty() == false)
            {
                // 원격의 기본 브랜치다. symref가 없으면 일반 ref가 아니라 알 수 없는 형태다.
                const std::string remote_prefix { std::string { "refs/remotes/" }.append(remote).append("/") };
                if (git_text::starts_with(symref, remote_prefix) && symref.size() > remote_prefix.size())
                    result.remote_defaults.push_back(remote_default_branch { std::string { remote }, std::string { symref.substr(remote_prefix.size()) } });
                else
                    ++result.skipped_count;
                continue;
            }

            ref entry {};
            entry.full_name = std::string { refname };
            entry.target_sha = std::string { object_name };
            if (git_text::starts_with(refname, "refs/heads/"))
            {
                entry.kind = ref_kind::local;
                entry.name = std::string { refname.substr(11) };
                entry.is_current = head_marker == "*";
                entry.upstream = std::string { upstream_short };
                if (upstream_short.empty() == false)
                {
                    entry.ahead = read_track_count(track, "ahead ");
                    entry.behind = read_track_count(track, "behind ");
                    // 문구가 "gone"인 경우도 인정하지만, 지역화를 피하려고 목록 안의 추적 ref
                    // 유무를 아래 후처리에서 다시 확인한다.
                    entry.upstream_gone = track.find("gone") != std::string_view::npos;
                }
            }
            else if (git_text::starts_with(refname, "refs/remotes/"))
            {
                entry.kind = ref_kind::remote;
                entry.name = std::string { refname.substr(13) };
            }
            else if (git_text::starts_with(refname, "refs/tags/"))
            {
                entry.kind = ref_kind::tag;
                entry.name = std::string { refname.substr(10) };
                // annotated 태그는 `*objectname`이 태그가 가리키는 커밋이다.
                if (peeled_name.empty() == false)
                {
                    entry.is_annotated_tag = true;
                    entry.target_sha = std::string { peeled_name };
                }
            }
            else if (refname == "refs/stash")
            {
                entry.kind = ref_kind::stash;
                entry.name = "stash";
            }
            else
            {
                ++result.skipped_count;
                continue;
            }
            result.refs.push_back(std::move(entry));
            // gone 판정은 모든 ref를 읽은 뒤에 한다. upstream 전체 이름은 입력 바이트를
            // 가리키는 view로만 들고 있다가 이 함수 안에서 쓰고 버린다.
            upstream_fulls.push_back(upstream_full);
        }

        // upstream으로 설정된 ref가 목록에 없으면 gone이다.
        std::unordered_set<std::string_view> existing {};
        existing.reserve(result.refs.size());
        for (const ref& entry : result.refs)
            existing.insert(entry.full_name);
        for (std::size_t index { 0 }; index < result.refs.size(); ++index)
        {
            ref& entry { result.refs[index] };
            if (entry.kind == ref_kind::local && upstream_fulls[index].empty() == false && existing.contains(upstream_fulls[index]) == false)
                entry.upstream_gone = true;
            if (entry.upstream_gone)
            {
                entry.ahead = 0;
                entry.behind = 0;
            }
        }
        return result;
    }

    std::vector<remote_tag> parse_ls_remote_tags(const std::string_view bytes)
    {
        std::vector<remote_tag> tags {};
        std::unordered_map<std::string, std::size_t> index_by_name {};
        std::string_view rest { bytes };

        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);

            std::string_view sha {};
            std::string_view name {};
            if (git_text::take_field(line, '\t', sha) == false)
                continue;
            name = line;
            if (git_text::starts_with(name, "refs/tags/") == false)
                continue;
            name.remove_prefix(10);

            // `^{}` 줄은 annotated 태그가 가리키는 커밋이다. 앞선 태그 객체 줄을 덮는다.
            const bool is_peeled { git_text::ends_with(name, "^{}") };
            if (is_peeled)
                name.remove_suffix(3);

            const auto found { index_by_name.find(std::string { name }) };
            if (found != index_by_name.end())
            {
                if (is_peeled)
                    tags[found->second].sha = std::string { sha };
                continue;
            }
            index_by_name.emplace(std::string { name }, tags.size());
            tags.push_back(remote_tag { std::string { name }, std::string { sha } });
        }
        return tags;
    }

    void apply_remote_tags(const std::span<ref> refs, const std::span<const remote_tag> remote_tags)
    {
        std::unordered_map<std::string_view, std::string_view> sha_by_name {};
        sha_by_name.reserve(remote_tags.size());
        for (const remote_tag& tag : remote_tags)
            sha_by_name.emplace(tag.name, tag.sha);

        for (ref& entry : refs)
        {
            if (entry.kind != ref_kind::tag)
                continue;
            const auto found { sha_by_name.find(entry.name) };
            entry.exists_on_remote = found != sha_by_name.end() && found->second == entry.target_sha;
        }
    }
} // namespace cyella
