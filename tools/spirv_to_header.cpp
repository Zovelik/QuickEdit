#include <fstream>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <filesystem>


std::string sanitize_name(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += c;
        }
        else {
            result += '_';
        }
    }
    return result;
}


int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: spirv_to_header <input.spv> <output.h>\n";
        return 1;
    }

    std::filesystem::path input_path = argv[1];
    std::filesystem::path output_path = argv[2];

    // Var name base on filename
    std::string var_name = sanitize_name(input_path.stem().string());

    // Read binary file
    std::ifstream file(input_path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open input file: " << input_path << "\n";
        return 1;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    file.close();

    // Padding to align on 4 bytes
    while (bytes.size() % 4 != 0) {
        bytes.push_back(0);
    }

    size_t word_count = bytes.size() / 4;

    // Convert to uint32_t
    std::vector<uint32_t> words(word_count);
    std::memcpy(words.data(), bytes.data(), bytes.size());

    // Write header
    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Failed to open output file: " << output_path << "\n";
        return 1;
    }

    out << "#pragma once\n\n";
    out << "#include <cstdint>\n\n";

    out << "alignas(4) static const uint32_t " << var_name << "[] = {\n";

    for (size_t i = 0; i < words.size(); ++i) {
        out << "0x" << std::hex << words[i] << std::dec;
        if (i != words.size() - 1)
            out << ",";
        out << "\n";
    }

    out << "};\n\n";
    out << "static const size_t " << var_name << "_size = sizeof(" << var_name << ");\n";

    out.close();

    std::cout << "Generated " << output_path << " (" << word_count << " words)\n";

    return 0;
}