#!/usr/bin/env python3
"""Exercise bounded WAV persistence, including damaged and oversized clips."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include "audio/recorder_file.h"
#include <cassert>
#include <vector>
int main(int argc,char** argv) {
    assert(argc==2); const std::string path=argv[1];
    std::vector<int16_t> samples(16000*30), restored(samples.size());
    for(size_t i=0;i<samples.size();++i) samples[i]=static_cast<int16_t>(i*13);
    assert(audio::SaveRecording(path,samples.data(),samples.size(),16000));
    assert(audio::LoadRecording(path,restored.data(),restored.size(),16000)==samples.size());
    assert(samples==restored);
    assert(!audio::SaveRecording(path,samples.data(),samples.size()+1,16000));
    assert(!audio::SaveRecording(path,samples.data(),0,16000));
    assert(audio::LoadRecording(path,restored.data(),100,16000)==0);
    assert(audio::LoadRecording(path,restored.data(),restored.size(),24000)==0);
    assert(audio::LoadRecording(path,restored.data(),restored.size(),16000)==samples.size());
    FILE* f=std::fopen(path.c_str(),"r+b"); assert(f);
    std::fseek(f,22,SEEK_SET); std::fputc(2,f); std::fclose(f);
    assert(audio::LoadRecording(path,restored.data(),restored.size(),16000)==0);
    assert(audio::SaveRecording(path,samples.data(),3200,16000));
    assert(truncate(path.c_str(),100)==0);
    assert(audio::LoadRecording(path,restored.data(),restored.size(),16000)==0);
    assert(!audio::SaveRecording(path+"/missing",samples.data(),3200,16000));
    std::puts("Recorder WAV OK: 30s roundtrip, size/rate/channel validation, truncated files, save failure");
}
'''
with tempfile.TemporaryDirectory(prefix="miaoink-recorder-") as temp:
    directory=Path(temp)
    source=directory/"test.cc"
    source.write_text(TEST)
    subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-fsanitize=undefined","-I",str(ROOT/"main"),str(source),"-o",str(directory/"test")],check=True)
    subprocess.run([str(directory/"test"),str(directory/"clip.wav")],check=True)
