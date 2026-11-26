"""Exercise the actual ImGui start body with an authoritative backend mode."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (args.source or root/'src/common/backends/imgui/launcher_imgui.cpp').read_text(encoding='utf-8')
    fixture = r"""
#include "launcher_netplay_delay.h"
#include <cstdio>
#include <cstring>
struct Settings {};
struct LauncherModel {
    bool netplay_rollback=true, netplay_manual_input_delay=false;
    bool netplay_manual_input_prediction=false;
    int netplay_lobby_input_delay=13, netplay_lobby_input_prediction=14;
    int launches=0; char netplay_status[256]{}; Settings s;
};
struct Backend {bool supported=false, actual=false;int delay=13,prediction=14,delay_writes=0,prediction_writes=0,starts=0;};
struct RecompLauncherCNetplayCallbacks {
    void* ctx;
    int (*rollback_set)(void*,int); int (*rollback_get)(void*);
    int (*input_delay_set)(void*,int); int (*input_prediction_set)(void*,int);
    int (*set_ready)(void*,int); int (*lobby_mods_missing)(void*);
    int (*request_start)(void*,const Settings*); int (*launch_pending)(void*);
};
static int set_mode(void* p,int requested) {auto& b=*(Backend*)p;b.actual=requested && b.supported;return 0;}
static int get_mode(void* p) {return ((Backend*)p)->actual;}
static int set_delay(void* p,int value) {auto& b=*(Backend*)p;b.delay=value;++b.delay_writes;return 0;}
static int set_prediction(void* p,int value) {auto& b=*(Backend*)p;b.prediction=value;++b.prediction_writes;return 0;}
static int ready(void*,int) {return 0;}
static int missing(void*) {return 0;}
static int start(void* p,const Settings*) {++((Backend*)p)->starts;return 0;}
static int pending(void*) {return 1;}
static int np_lobby_max_peer_rtt_ms(LauncherModel*,const RecompLauncherCNetplayCallbacks*) {return 146;}
static void np_try_launch(LauncherModel* m) {++m->launches;}
"""
    for name in ['static int np_delay_frames_from_rtt_ms(',
                 'static int np_rb_delay_frames_from_rtt_ms(',
                 'static int np_rb_prediction_frames_from_rtt_ms(',
                 'static void np_lobby_start(']:
        fixture += function(source, name) + '\n'
    fixture += r"""
static int failures;
static void check(bool ok,const char* label) {if(!ok) {std::fprintf(stderr,"FAIL %s\n",label);++failures;}}
static void case_start(bool supported,bool getter,bool manual_delay,bool manual_prediction) {
    LauncherModel m;Backend b;b.supported=supported;
    m.netplay_manual_input_delay=manual_delay;m.netplay_manual_input_prediction=manual_prediction;
    RecompLauncherCNetplayCallbacks cb{&b,set_mode,getter?get_mode:nullptr,set_delay,set_prediction,ready,missing,start,pending};
    np_lobby_start(&m,&cb);
    const bool use_rb=getter?supported:true;
    const int expected=manual_delay?13:(use_rb?8:12); // literal independent 146ms controls
    check(m.netplay_rollback==use_rb,"model mirrors effective mode; absent getter retains legacy request");
    check(m.netplay_lobby_input_delay==expected && b.delay==expected,"actual start selects effective-mode Auto delay");
    check(b.delay_writes==(manual_delay?0:1),"manual delay is not overwritten");
    check(b.prediction_writes==((use_rb && !manual_prediction)?1:0),"Auto prediction only in effective rollback mode");
    if(!use_rb || manual_prediction)check(b.prediction==14,"manual/nonrollback prediction preserved");
    check(b.starts==1 && m.launches==1 && !m.netplay_status[0],"normal start/launch path retained");
}
int main() {
    case_start(false,true,false,false);
    case_start(true,true,false,false);
    case_start(false,true,true,false);
    case_start(true,true,true,true);
    case_start(false,false,false,false);
    return failures?1:0;
}
"""
    with tempfile.TemporaryDirectory(prefix='recomp-ui-start-') as temporary:
        cpp=Path(temporary)/'start.cpp'
        binary=Path(temporary)/('start.exe' if os.name=='nt' else 'start')
        cpp.write_text(fixture,encoding='utf-8',newline='\n')
        if Path(args.compiler).name.lower() in ('cl','cl.exe'):
            command=[args.compiler,'/nologo','/std:c++17','/EHsc','/I'+str(root/'src/common'),str(cpp),'/Fe:'+str(binary)]
        else:
            command=[args.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'src/common'),str(cpp),'-o',str(binary)]
        subprocess.run(command,check=True,cwd=temporary)
        result=subprocess.run([str(binary)],cwd=temporary)
        if result.returncode:return result.returncode
    print('actual start: clamped mode, rollback, manual delay/prediction and optional getter: PASS')
    return 0


if __name__=='__main__':
    raise SystemExit(main())
