#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/cli_tab.h"

#include <string>
#include <vector>

using zg::ui::run_cli_command;

TEST_CASE("cli: blank and whitespace-only lines are ignored") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("", sb));
    CHECK_FALSE(run_cli_command("   \t ", sb));
    CHECK(sb.empty());
}

TEST_CASE("cli: /panic fires and echoes what it is about to do") {
    std::vector<std::string> sb;
    CHECK(run_cli_command("/panic", sb));
    REQUIRE(sb.size() == 2);
    CHECK(sb[0] == "> /panic");
    CHECK(sb[1].find("wiping") != std::string::npos);
}

TEST_CASE("cli: surrounding whitespace doesn't defeat a command") {
    std::vector<std::string> sb;
    CHECK(run_cli_command("  /panic \t", sb));
}

TEST_CASE("cli: /help lists /panic and never fires") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/help", sb));
    bool mentions_panic = false;
    for (const auto& l : sb) {
        if (l.find("/panic") != std::string::npos) mentions_panic = true;
    }
    CHECK(mentions_panic);
}

TEST_CASE("cli: unknown commands report themselves and never fire") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/wat", sb));
    REQUIRE(sb.size() == 2);
    CHECK(sb[0] == "> /wat");
    CHECK(sb[1].find("unknown command: /wat") != std::string::npos);
}

TEST_CASE("cli: a near-miss of /panic does not fire") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/panic now", sb));
    CHECK_FALSE(run_cli_command("panic", sb));
}
