#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/**
 * @brief WAV 文件写入工具
 * 
 * 将 float32 PCM 数据写入 16-bit 单声道 WAV 文件。
 * 用于 TTS 调试时保存音频。
 */
class WavWriter {
public:
    /**
     * @brief 将 float32 PCM 写入 WAV 文件
     * @param path 输出文件路径
     * @param samples float32 PCM 数据 (范围 -1.0 ~ 1.0)
     * @param numSamples 采样数
     * @param sampleRate 采样率 (如 44100)
     * @return true 成功
     */
    static bool WriteWav(const std::string& path,
                         const float* samples, int numSamples,
                         int sampleRate) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;

        int bytesPerSample = 2;  // 16-bit
        int numChannels = 1;
        int dataSize = numSamples * bytesPerSample;
        int fileSize = 36 + dataSize;

        // RIFF header
        fwrite("RIFF", 1, 4, f);
        fwrite(&fileSize, 4, 1, f);
        fwrite("WAVE", 1, 4, f);

        // fmt chunk
        fwrite("fmt ", 1, 4, f);
        int fmtSize = 16;
        short audioFormat = 1;       // PCM
        short channels = numChannels;
        int byteRate = sampleRate * channels * bytesPerSample;
        short blockAlign = channels * bytesPerSample;
        short bitsPerSample = 16;

        fwrite(&fmtSize, 4, 1, f);
        fwrite(&audioFormat, 2, 1, f);
        fwrite(&channels, 2, 1, f);
        fwrite(&sampleRate, 4, 1, f);
        fwrite(&byteRate, 4, 1, f);
        fwrite(&blockAlign, 2, 1, f);
        fwrite(&bitsPerSample, 2, 1, f);

        // data chunk
        fwrite("data", 1, 4, f);
        fwrite(&dataSize, 4, 1, f);

        // float32 → int16 PCM
        std::vector<int16_t> pcm(numSamples);
        for (int i = 0; i < numSamples; i++) {
            float s = samples[i];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            pcm[i] = static_cast<int16_t>(s * 32767.0f);
        }
        fwrite(pcm.data(), sizeof(int16_t), numSamples, f);

        fclose(f);
        return true;
    }

    /**
     * @brief 将 int16 PCM 写入 WAV 文件
     */
    static bool WriteWavInt16(const std::string& path,
                              const int16_t* samples, int numSamples,
                              int sampleRate) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;

        int bytesPerSample = 2;
        int numChannels = 1;
        int dataSize = numSamples * bytesPerSample;
        int fileSize = 36 + dataSize;

        fwrite("RIFF", 1, 4, f);
        fwrite(&fileSize, 4, 1, f);
        fwrite("WAVE", 1, 4, f);

        fwrite("fmt ", 1, 4, f);
        int fmtSize = 16;
        short audioFormat = 1;
        short channels = numChannels;
        int byteRate = sampleRate * channels * bytesPerSample;
        short blockAlign = channels * bytesPerSample;
        short bitsPerSample = 16;

        fwrite(&fmtSize, 4, 1, f);
        fwrite(&audioFormat, 2, 1, f);
        fwrite(&channels, 2, 1, f);
        fwrite(&sampleRate, 4, 1, f);
        fwrite(&byteRate, 4, 1, f);
        fwrite(&blockAlign, 2, 1, f);
        fwrite(&bitsPerSample, 2, 1, f);

        fwrite("data", 1, 4, f);
        fwrite(&dataSize, 4, 1, f);
        fwrite(samples, sizeof(int16_t), numSamples, f);

        fclose(f);
        return true;
    }
};

#endif // WAV_WRITER_H
