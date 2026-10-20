#include "presentation/file_tree.h"

#include <map>
#include <string_view>

namespace cyella {
    namespace {
        struct folder_node
        {
            // 이름 순으로 서도록 map이다. 같은 이름이 둘이면 한 폴더다.
            std::map<std::string, folder_node> folders {};
            // (이름, 입력 번호)다.
            std::multimap<std::string, std::size_t> files {};
            std::size_t file_count { 0 };
        };

        std::u8string to_u8(const std::string_view text)
        {
            return { text.begin(), text.end() };
        }

        void emit(const folder_node& node, const std::string& prefix, const std::size_t depth, const std::set<std::string>& collapsed, std::vector<file_tree_row>& rows)
        {
            for (const auto& [name, child] : node.folders)
            {
                // 파일 없이 하위 폴더 하나만 있는 사슬은 한 행으로 합친다.
                const folder_node* folder { &child };
                std::string label { name };
                while (folder->files.empty() && folder->folders.size() == 1)
                {
                    const auto& only { *folder->folders.begin() };
                    label += '/' + only.first;
                    folder = &only.second;
                }
                const std::string path { prefix + label };
                file_tree_row row {};
                row.folder = true;
                row.key = u8"d:" + to_u8(path);
                row.label = to_u8(label);
                row.path = path;
                row.depth = depth;
                row.file_count = folder->file_count;
                row.expanded = collapsed.contains(path) == false;
                rows.push_back(std::move(row));
                if (rows.back().expanded)
                    emit(*folder, path + '/', depth + 1, collapsed, rows);
            }
            for (const auto& [name, index] : node.files)
            {
                file_tree_row row {};
                row.label = to_u8(name);
                row.depth = depth;
                row.file_index = index;
                rows.push_back(std::move(row));
            }
        }
    } // namespace

    std::vector<file_tree_row> build_file_tree(const std::vector<std::string>& paths, const std::set<std::string>& collapsed)
    {
        folder_node root {};
        for (std::size_t index { 0 }; index < paths.size(); ++index)
        {
            const std::string_view path { paths[index] };
            folder_node* node { &root };
            ++node->file_count;
            std::size_t start { 0 };
            for (std::size_t slash { path.find('/') }; slash != std::string_view::npos; slash = path.find('/', start))
            {
                // 빈 구간(`a//b`, 앞의 `/`)은 폴더가 아니다.
                if (slash > start)
                {
                    node = &node->folders[std::string { path.substr(start, slash - start) }];
                    ++node->file_count;
                }
                start = slash + 1;
            }
            node->files.emplace(std::string { path.substr(start) }, index);
        }
        std::vector<file_tree_row> rows {};
        rows.reserve(paths.size());
        emit(root, {}, 0, collapsed, rows);
        return rows;
    }
} // namespace cyella
