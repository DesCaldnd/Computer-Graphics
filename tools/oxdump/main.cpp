// oxdump — inspect and convert OXB1 archives (.oxscene, .oxprefab, .oxsave, ...) without knowing C++ types.
//
//   oxdump <file> [-o out.json] [--indent N]   binary (or JSON) archive -> pretty JSON
//   oxdump --to-binary <in.json> <out>          JSON archive -> OXB1
//   oxdump --info <file>                        header, chunks (CRC), schema table
//   oxdump --check <file>                       CRC + lossless binary -> JSON -> binary verification

#include <oxwald/core/serial/format.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ox;

int usage() {
    std::fputs("usage:\n"
               "  oxdump <file> [-o <out.json>] [--indent N]\n"
               "  oxdump --to-binary <in.json> <out>\n"
               "  oxdump --info <file>\n"
               "  oxdump --check <file>\n",
               stderr);
    return 2;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "oxdump: %s\n", message.c_str());
    return 1;
}

int dumpJson(const std::string& input, const std::string& output, int indent) {
    auto bytes = serial::readFileBytes(input);
    if (!bytes) return fail(bytes.error().message);
    auto doc = serial::decodeAny(*bytes);
    if (!doc) return fail(input + ": " + doc.error().message);
    const std::string json = serial::toJsonString(*doc, indent) + "\n";
    if (output.empty()) {
        std::fwrite(json.data(), 1, json.size(), stdout);
        return 0;
    }
    auto st = serial::writeFileAtomic(output, std::as_bytes(std::span(json.data(), json.size())));
    return st ? 0 : fail(st.error().message);
}

int toBinary(const std::string& input, const std::string& output) {
    auto bytes = serial::readFileBytes(input);
    if (!bytes) return fail(bytes.error().message);
    auto bin = serial::jsonToBinary(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!bin) return fail(input + ": " + bin.error().message);
    auto st = serial::writeFileAtomic(output, *bin);
    return st ? 0 : fail(st.error().message);
}

int info(const std::string& input) {
    auto bytes = serial::readFileBytes(input);
    if (!bytes) return fail(bytes.error().message);
    auto i = serial::inspectBinary(*bytes);
    if (!i) return fail(input + ": " + i.error().message);
    std::printf("file           %s (%zu bytes)\n", input.c_str(), i->fileSize);
    std::printf("format         OXB1 v%u (flags 0x%04x)\n", unsigned(i->formatVersion), unsigned(i->flags));
    std::printf("kind           %s\n", i->kind.empty() ? "-" : i->kind.c_str());
    std::printf("data version   %u\n", i->dataVersion);
    std::printf("chunks         %zu\n", i->chunks.size());
    bool allOk = true;
    for (const auto& c : i->chunks) {
        std::printf("  %-4s  offset %8u  size %8u  crc %08x  %s\n", c.id.c_str(), c.offset, c.size, c.crc,
                    c.crcValid ? "crc ok" : "CRC MISMATCH");
        allOk = allOk && c.crcValid;
    }
    std::printf("strings        %zu\n", i->strings.size());
    std::printf("types          %zu\n", i->types.size());
    for (usize t = 0; t < i->types.size(); ++t) {
        const auto& type = i->types[t];
        std::printf("  [%zu] %s  (hash %016llx, %zu fields)\n", t, type.name.empty() ? "<anonymous>" : type.name.c_str(),
                    static_cast<unsigned long long>(type.nameHash), type.fields.size());
        for (const auto& f : type.fields) {
            std::printf("        %-28s %s\n", f.name.c_str(), f.desc.c_str());
        }
    }
    return allOk ? 0 : 1;
}

int check(const std::string& input) {
    auto bytes = serial::readFileBytes(input);
    if (!bytes) return fail(bytes.error().message);
    if (!serial::isBinaryArchive(*bytes)) return fail(input + ": not an OXB1 archive");
    auto json = serial::binaryToJson(*bytes);
    if (!json) return fail(input + ": " + json.error().message);
    auto back = serial::jsonToBinary(*json);
    if (!back) return fail("JSON re-encode failed: " + back.error().message);
    if (*back != *bytes) return fail("binary -> JSON -> binary is not byte-identical");
    std::printf("%s: OK (%zu bytes, CRCs valid, lossless JSON round trip)\n", input.c_str(), bytes->size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    if (args[0] == "-h" || args[0] == "--help") {
        usage();
        return 0;
    }
    if (args[0] == "--to-binary") return args.size() == 3 ? toBinary(args[1], args[2]) : usage();
    if (args[0] == "--info") return args.size() == 2 ? info(args[1]) : usage();
    if (args[0] == "--check") return args.size() == 2 ? check(args[1]) : usage();

    std::string input;
    std::string output;
    int indent = 2;
    for (usize i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) {
            output = args[++i];
        } else if (args[i] == "--indent" && i + 1 < args.size()) {
            indent = std::atoi(args[++i].c_str());
        } else if (!args[i].empty() && args[i][0] == '-') {
            return usage();
        } else if (input.empty()) {
            input = args[i];
        } else {
            return usage();
        }
    }
    if (input.empty()) return usage();
    return dumpJson(input, output, indent);
}
