#include "json_reader.h"
#include "json_writer.h"
#include "model_config_reader.h"
#include "prediction_runner.h"
#include "qualifier_pipeline.h"

#include <exception>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class CommandLineError : public std::runtime_error {
public:
    explicit CommandLineError(const std::string& message)
        : std::runtime_error(message) {}
};

void printUsage(std::ostream& output, const char* executable) {
    output
        << "Usage:\n"
        << "  " << executable
        << " predict --model <config.json> --input <request.json> "
           "(--output <response.json> | --stdout) [--compact]\n\n"
        << "Modes:\n"
        << "  predict   Run the frozen seven-layer model for one schema-3 "
           "request.\n"
        << "  sandbox   Reserved for the P1 training pipeline; not available "
           "yet.\n\n"
        << "Options:\n"
        << "  --model    Frozen or smoke-test model configuration.\n"
        << "  --input    UTF-8 schema-3 request JSON.\n"
        << "  --output   Destination JSON file.\n"
        << "  --stdout   Write only the result JSON to standard output.\n"
        << "  --compact  Disable indentation and line breaks.\n"
        << "  --help     Show this help text.\n";
}

std::string requireValue(int argc,
                         char* argv[],
                         int& index,
                         const std::string& option) {
    if (index + 1 >= argc) {
        throw CommandLineError(option + " requires a value");
    }
    ++index;
    const std::string value = argv[index];
    if (value.empty() || value.rfind("--", 0) == 0) {
        throw CommandLineError(option + " requires a non-empty value");
    }
    return value;
}

chongming::qualifier::PredictionOptions parsePredictOptions(
    int argc, char* argv[]) {
    chongming::qualifier::PredictionOptions result;
    bool has_model = false;
    bool has_input = false;
    bool has_output = false;

    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--model") {
            if (has_model) throw CommandLineError("duplicate --model option");
            result.model_path =
                std::filesystem::path(requireValue(argc, argv, index, option));
            has_model = true;
        } else if (option == "--input") {
            if (has_input) throw CommandLineError("duplicate --input option");
            result.input_path =
                std::filesystem::path(requireValue(argc, argv, index, option));
            has_input = true;
        } else if (option == "--output") {
            if (has_output) throw CommandLineError("duplicate --output option");
            result.output_path =
                std::filesystem::path(requireValue(argc, argv, index, option));
            has_output = true;
        } else if (option == "--stdout") {
            if (result.write_stdout) {
                throw CommandLineError("duplicate --stdout option");
            }
            result.write_stdout = true;
        } else if (option == "--compact") {
            result.pretty_json = false;
        } else if (option == "--help") {
            printUsage(std::cout, argv[0]);
            std::exit(0);
        } else {
            throw CommandLineError("unknown predict option: " + option);
        }
    }

    if (!has_model) throw CommandLineError("predict requires --model");
    if (!has_input) throw CommandLineError("predict requires --input");
    if (has_output == result.write_stdout) {
        throw CommandLineError(
            "predict requires exactly one of --output and --stdout");
    }
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(std::cerr, argv[0]);
        return 1;
    }

    const std::string mode = argv[1];
    if (mode == "--help" || mode == "-h") {
        printUsage(std::cout, argv[0]);
        return 0;
    }

    try {
        if (mode == "predict") {
            const auto options = parsePredictOptions(argc, argv);
            chongming::qualifier::PredictionRunner::run(options, std::cout);
            return 0;
        }
        if (mode == "sandbox") {
            throw CommandLineError(
                "sandbox is a reserved P1 mode and is not implemented yet");
        }
        throw CommandLineError("unknown mode: " + mode);
    } catch (const CommandLineError& error) {
        std::cerr << "command line error: " << error.what() << '\n';
        return 1;
    } catch (const chongming::qualifier::JsonReadError& error) {
        std::cerr << "input JSON error: " << error.what() << '\n';
        return 2;
    } catch (const chongming::qualifier::ModelConfigError& error) {
        std::cerr << "model config error: " << error.what() << '\n';
        return 3;
    } catch (const chongming::qualifier::JsonWriteError& error) {
        std::cerr << "output JSON error: " << error.what() << '\n';
        return 5;
    } catch (const std::exception& error) {
        std::cerr << "algorithm error: " << error.what() << '\n';
        return 4;
    }
}
