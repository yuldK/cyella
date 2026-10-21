#include "domain/ssh_connection.h"

#include "domain/text_catalog.h"

namespace cyella {
    std::u8string describe_ssh_connection(const ssh_connection_classification& classification)
    {
        switch (classification.result)
        {
        case ssh_connection_result::authenticated:
            if (classification.authenticated_user.empty())
                return tr(u8"ssh_connection.authenticated");
            return tr(u8"ssh_connection.authenticated_as", { { u8"user", classification.authenticated_user } });
        case ssh_connection_result::permission_denied:
            return tr(u8"ssh_connection.permission_denied");
        case ssh_connection_result::host_key_rejected:
            return tr(u8"ssh_connection.host_key_rejected");
        case ssh_connection_result::host_key_changed:
            return tr(u8"ssh_connection.host_key_changed");
        case ssh_connection_result::connection_failed:
            return tr(u8"ssh_connection.connection_failed");
        case ssh_connection_result::unknown:
            break;
        }
        return tr(u8"ssh_connection.unknown");
    }
} // namespace cyella
