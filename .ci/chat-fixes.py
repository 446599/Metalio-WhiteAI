from pathlib import Path

def replace(path, old, new):
    p=Path(path); text=p.read_text(); assert text.count(old)==1,(path,old[:100]); p.write_text(text.replace(old,new,1))

replace('main/chat/history_service.cc',
        'job=queue_.front();result=view_;}',
        'job=queue_.front();result=view_;view_.busy=true;} // Block UI queue reordering during SD I/O.')
replace('tools/tests/mono_stubs/test_platform.h',
        ' bool sd=true,wifi=true,connected=true,ble_ok=true;',
        ' mutable std::function<void()> sd_probe;\n bool sd=true,wifi=true,connected=true,ble_ok=true;')
replace('tools/tests/mono_stubs/test_platform.h',
        'bool IsSdMounted()const{return sd;}',
        'bool IsSdMounted()const{if(sd_probe)sd_probe();return sd;}')
replace('tools/tests/chat_history_contract.cc',
        'CHECK(history.Snapshot().pending==1);CHECK(!history.Switch(0));history.Poll(100);',
        '''CHECK(history.Snapshot().pending==1);CHECK(!history.Switch(0));
    GetHAL().sd_probe=[&](){CHECK(history.Snapshot().busy);CHECK(!history.List());};
    history.Poll(100);GetHAL().sd_probe={};''')
replace('main/xiaozhi/xiaozhi_client.cc',
        'action_label_.clear(); task_expected_=false;ChatTask::Instance().Cancel();',
        'action_label_.clear(); resume_context_.clear();task_expected_=false;ChatTask::Instance().Cancel();')
replace('main/xiaozhi/xiaozhi_client.cc',
        'last_text_action_ms_=now;++intent_generation_;action_label_=label;task_expected_=true;',
        'last_text_action_ms_=now;++intent_generation_;action_label_=label;task_expected_=true;\n    if(label!="继续对话")resume_context_.clear();')
replace('main/chat/history_service.h','bool TakeResumed(std::string& context);','bool TakeResumed(std::string& context);\n    void CancelResume();')
replace('main/chat/history_service.cc','void History::Poll(int64_t now){','void History::CancelResume(){std::lock_guard<std::mutex> lock(mutex_);resume_ready_=false;resume_context_.clear();}\nvoid History::Poll(int64_t now){')
replace('main/xiaozhi/xiaozhi_client.cc','action_label_.clear(); resume_context_.clear();task_expected_=false;ChatTask::Instance().Cancel();','action_label_.clear(); resume_context_.clear();chat::History::Instance().CancelResume();task_expected_=false;ChatTask::Instance().Cancel();')
replace('main/xiaozhi/xiaozhi_client.cc','if(label!="继续对话")resume_context_.clear();','if(label!="继续对话"){resume_context_.clear();chat::History::Instance().CancelResume();}')
replace('main/xiaozhi/xiaozhi_client.cc','''        std::string context;
        if(chat::History::Instance().TakeResumed(context)){std::lock_guard<std::mutex> lock(intent_mutex_);resume_context_=std::move(context);}
        {
            std::lock_guard<std::mutex> lock(intent_mutex_);''','''        {
            std::lock_guard<std::mutex> lock(intent_mutex_);
            std::string context;
            if(chat::History::Instance().TakeResumed(context))resume_context_=std::move(context);''')
replace('tools/tests/chat_client_history_stub.h','inline bool History::CanCapture()const{return true;}','inline bool History::CanCapture()const{return true;}\ninline void History::CancelResume(){}')
replace('tools/tests/chat_history_contract.cc','CHECK(history.Switch(0));history.Poll(107);','CHECK(history.Switch(1));history.Poll(106);history.CancelResume();CHECK(!history.TakeResumed(resumed));\n    CHECK(history.Switch(0));history.Poll(107);')
