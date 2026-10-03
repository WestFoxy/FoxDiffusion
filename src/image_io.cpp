#include "image_io.hpp"
#include "raster.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <fstream>

std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& rgba, int width, int height) {
    std::vector<std::uint8_t> result;
    if(width <= 0 || height <= 0 || rgba.size() != size_t(width)*height*4) return result;
    stbi_write_png_to_func([](void* context, void* data, int size) {
        auto& bytes = *static_cast<std::vector<std::uint8_t>*>(context);
        auto first = static_cast<std::uint8_t*>(data); bytes.insert(bytes.end(), first, first+size);
    }, &result, width, height, 4, rgba.data(), width*4);
    return result;
}

bool save_png_parameters(const std::filesystem::path& path,const std::vector<std::uint8_t>& rgba,int width,int height,const std::string& parameters,std::string& error) {
    auto png=encode_png(rgba,width,height);if(png.size()<12){error="PNG encoding failed";return false;}
    std::vector<std::uint8_t> chunk;
    auto be=[&](std::uint32_t n){for(int shift=24;shift>=0;shift-=8)chunk.push_back(std::uint8_t(n>>shift));};
    const std::string content=std::string("parameters\0",11)+std::string(4,'\0')+parameters;
    if(content.size()>16*1024*1024){error="PNG metadata is too large";return false;}
    be(std::uint32_t(content.size()));for(char c:std::string("iTXt"))chunk.push_back(c);chunk.insert(chunk.end(),content.begin(),content.end());
    std::uint32_t crc=0xffffffff;for(size_t i=4;i<chunk.size();++i){crc^=chunk[i];for(int b=0;b<8;++b)crc=(crc>>1)^(0xedb88320u&std::uint32_t(-int(crc&1)));}be(crc^0xffffffff);
    png.insert(png.end()-12,chunk.begin(),chunk.end());std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(png.data()),png.size());file.flush();
    if(!file){error="Could not write PNG with metadata";return false;}return true;
}
std::string read_png_parameters(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);unsigned char signature[8]{};file.read(reinterpret_cast<char*>(signature),8);
    if(!file||signature[0]!=137||std::string(reinterpret_cast<char*>(signature+1),3)!="PNG")return {};
    for(int chunks=0;chunks<10000&&file;++chunks){unsigned char header[8]{};file.read(reinterpret_cast<char*>(header),8);if(!file)break;
        std::uint32_t size=(std::uint32_t(header[0])<<24)|(std::uint32_t(header[1])<<16)|(std::uint32_t(header[2])<<8)|header[3];
        std::string type(reinterpret_cast<char*>(header+4),4);if(type=="IEND")break;
        if(type=="tEXt"&&size<16*1024*1024){std::string text(size,'\0');file.read(text.data(),size);if(!file)return {};if(text.starts_with(std::string("parameters\0",11)))return text.substr(11);file.seekg(4,std::ios::cur);}
        else if(type=="iTXt"&&size<16*1024*1024){std::string text(size,'\0');file.read(text.data(),size);if(!file)return {};if(text.starts_with(std::string("parameters\0",11))&&text.size()>13&&text[11]==0){size_t language=text.find('\0',13);size_t translated=language==std::string::npos?language:text.find('\0',language+1);if(translated!=std::string::npos)return text.substr(translated+1);}file.seekg(4,std::ios::cur);}
        else file.seekg(std::streamoff(size)+4,std::ios::cur);
    }return {};
}

bool save_image(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& rgba,
                int width,
                int height,
                int jpeg_quality,
                std::string& error) {
    if (width <= 0 || height <= 0 || uint64_t(width)*height>uint64_t(INT_MAX)/4 || rgba.size() < size_t(width)*height*4) {
        error = "There is no generated image to save.";
        return false;
    }

    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    int result = 0;
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    if(!file){error="Cannot open the output file.";return false;}
    auto write=[](void* context,void* data,int size){static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data),size);};
    if (extension == ".png") {
        result = stbi_write_png_to_func(write,&file, width, height, 4, rgba.data(), width * 4);
    } else if (extension == ".jpg" || extension == ".jpeg") {
        result = stbi_write_jpg_to_func(write,&file, width, height, 4, rgba.data(),
                                std::clamp(jpeg_quality, 1, 100));
    } else {
        error = "Choose a .png, .jpg, or .jpeg file extension.";
        return false;
    }
    file.flush();
    if (!result||!file) error = "The image encoder could not write the selected file.";
    return result != 0 && bool(file);
}

bool load_image_rgba(const std::filesystem::path& path,
                     std::vector<std::uint8_t>& rgba,
                     int& width,
                     int& height,
                     std::string& error) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file){error="Could not open the image.";return false;}
    auto size=file.tellg();if(size<=0||size>INT_MAX){error="Invalid or oversized image file.";return false;}
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
    if(!file){error="Could not read the image.";return false;}
    return load_image_rgba_memory(bytes,rgba,width,height,error);
}

bool load_image_rgba_memory(const std::vector<std::uint8_t>& encoded,
                            std::vector<std::uint8_t>& rgba,
                            int& width,
                            int& height,
                            std::string& error) {
    if (encoded.empty()||encoded.size()>INT_MAX) {error="Invalid image data.";return false;}
    int channels = 0;
    int info_w=0,info_h=0;
    if(!stbi_info_from_memory(encoded.data(),int(encoded.size()),&info_w,&info_h,&channels)||info_w<=0||info_h<=0||uint64_t(info_w)*info_h>uint64_t(INT_MAX)/4){error="Invalid image dimensions or image exceeds codec addressable buffer.";return false;}
    try { require_image_memory(checked_image_bytes(info_w,info_h)*2); }
    catch(const std::exception& e) { error=e.what();return false; }
    stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                            &width, &height, &channels, 4);
    if (!pixels) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "Could not decode preview";
        return false;
    }
    rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}
