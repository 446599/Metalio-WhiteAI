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
# New explicit input supersedes an automatic resume not yet sent on reconnect.
replace('main/xiaozhi/xiaozhi_client.cc',
        'action_label_.clear(); task_expected_=false;ChatTask::Instance().Cancel();',
        'action_label_.clear(); resume_context_.clear();task_expected_=false;ChatTask::Instance().Cancel();')
replace('main/xiaozhi/xiaozhi_client.cc',
        'last_text_action_ms_=now;++intent_generation_;action_label_=label;task_expected_=true;',
        'last_text_action_ms_=now;++intent_generation_;action_label_=label;task_expected_=true;\n    if(label!="继续对话")resume_context_.clear();')
