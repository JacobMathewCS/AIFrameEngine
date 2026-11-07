#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
static void SaveArtifact(const std::string& name,const std::vector<unsigned char>& bytes) {
    const auto path=std::filesystem::path(name);
    if(std::filesystem::exists(path)) throw std::runtime_error("refuse artifact overwrite");
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    file.close(); if(!file) throw std::runtime_error("artifact write failed");
}
static void SaveMetrics(const std::string& stem,unsigned mode,unsigned width,unsigned height,
    unsigned maximum,size_t violations) {
    std::ostringstream text;
    text<<"case="<<mode<<" width="<<width<<" height="<<height<<" maximum="<<maximum
        <<" violations="<<violations<<" tolerance=1\n";
    const auto value=text.str(); SaveArtifact(stem+"_metrics.txt",std::vector<unsigned char>(value.begin(),value.end()));
}