#include <iostream>
#include <utility>
#include "logging.h"
#include "proto_con.h"
#include "send_packet_manager.h"
#include "byte_codec.h"
#include <string.h>
#include <stdint.h>
#include "fun_test.h"
#include "proto_framer.h"
#include "proto_time.h"
#include <vector>
#include "socket_address.h"
using namespace dqc;
int main(){
    su_platform_init();
    //SendPacketManager manager(nullptr);
    //manager.Test();
    //manager.Test2();
    test_test();
    su_platform_uninit();
}


