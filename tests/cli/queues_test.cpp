#include <rlbs/cli/queues.hpp>

#include <array>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

void test_list_and_add_parse() {
    const std::array<std::string_view, 0> list_arguments{};
    const auto list = rlbs::parse_queues_command(list_arguments);

    expect(list && list->action == rlbs::QueuesCommandAction::list,
           "empty queues command lists queues");

    const std::array<std::string_view, 8> add_arguments{
        "add",         "short",       "--priority", "100",
        "--max-running", "4",         "--socket",   "/tmp/custom.sock",
    };
    const auto add = rlbs::parse_queues_command(add_arguments);

    expect(add && add->action == rlbs::QueuesCommandAction::add,
           "queue add action parses");
    expect(add && add->name == "short", "queue add keeps its name");
    expect(add && add->priority == 100, "queue add keeps its priority");
    expect(add && add->max_running == 4,
           "queue add keeps its running limit");
    expect(add && add->socket_path == "/tmp/custom.sock",
           "queue add keeps its socket");
}

void test_switch_actions_parse() {
    const std::array<std::string_view, 2> stop_arguments{"stop", "short"};
    const std::array<std::string_view, 2> disable_arguments{"disable",
                                                            "short"};
    const auto stop = rlbs::parse_queues_command(stop_arguments);
    const auto disable = rlbs::parse_queues_command(disable_arguments);

    expect(stop && stop->action == rlbs::QueuesCommandAction::stop,
           "queue stop parses");
    expect(disable && disable->action == rlbs::QueuesCommandAction::disable,
           "queue disable parses");
}

void test_bad_commands_are_rejected() {
    const std::array<std::string_view, 1> missing_name{"start"};
    const std::array<std::string_view, 4> wrong_option{
        "stop", "short", "--priority", "10"};
    const std::array<std::string_view, 4> zero_limit{
        "add", "short", "--max-running", "0"};
    const std::array<std::string_view, 1> unknown{"remove"};

    expect(!rlbs::parse_queues_command(missing_name),
           "queue action requires a name");
    expect(!rlbs::parse_queues_command(wrong_option),
           "queue switch rejects add-only settings");
    expect(!rlbs::parse_queues_command(zero_limit),
           "queue add rejects zero running limit");
    expect(!rlbs::parse_queues_command(unknown),
           "unknown queue action is rejected");
}

void test_queue_table_format() {
    const std::vector<rlbs::BatchQueue> queues{
        {
            .name = "short",
            .priority = 100,
            .enabled = true,
            .started = false,
            .max_running = 4,
        },
        {
            .name = "default",
            .priority = 0,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        },
    };
    const auto output = rlbs::format_queues(queues);

    expect(output.contains("priority"), "queue table has a priority header");
    expect(output.contains("short"), "queue table prints queue names");
    expect(output.contains("unlimited"),
           "queue table explains an absent running limit");
}

} // namespace

int main() {
    test_list_and_add_parse();
    test_switch_actions_parse();
    test_bad_commands_are_rejected();
    test_queue_table_format();

    if (failures == 0) {
        std::cout << "all queues cli tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
