/* Shared optional LAN room adapter. Included after the ordinary backend's
 * callback table so online/legacy operations keep their existing paths. */
static RNetLanRoomPeer* r4_peer;
static RNetLanBeacon* r4_beacon;
static RNetLanRoom r4_last;
static RecompLauncherCNetplayLaunch r4_launch;
static int r4_active,r4_host,r4_reopen;
static char r4_endpoint[64],r4_bind[64],r4_password[RNET_LAN_LOBBY_PASSWORD_MAX];
static unsigned r4_published;
static RecompLauncherCNetplayChatMessage r4_chat[64];
static unsigned r4_chat_head,r4_chat_count,r4_chat_sequence;
static int room4_active(void){return r4_active;}
static const RNetLanRoom* room4_state(void){return r4_peer?rnet_lan_room_state(r4_peer):&r4_last;}
static int room4_count(const RNetLanRoom* r){int i,n=0;for(i=0;i<r->max_slots;++i)n+=r->member[i].id!=0;return n;}
static void room4_error(int error) {
  const char* text=error==RNET_LAN_ROOM_PASSWORD?"bad_password":error==RNET_LAN_ROOM_FULL?"lobby_full":
    error==RNET_LAN_ROOM_IDENTITY?"version_mismatch":error==RNET_LAN_ROOM_KICKED?"kicked":
    error==RNET_LAN_ROOM_TIMEOUT?"lan_room_timeout":error==RNET_LAN_ROOM_CLOSED?"host_left":"lan_room_failed";
  snprintf(g_runtime_error,sizeof(g_runtime_error),"%s",text);
}
static void room4_id(char* out,size_t cap,const char* endpoint){snprintf(out,cap,"lr4:%s",endpoint);}
static int room4_index(RecompLauncherCNetplayLobby* out) {
  FILE* f;char magic[32],endpoint[66],game[66],version[34],name[98];int players,max,pw;
  f=fopen(lan_path(),"rb");if(!f)return 0;
  if(!fgets(magic,sizeof(magic),f)||!fgets(endpoint,sizeof(endpoint),f)||!fgets(game,sizeof(game),f)
      ||!fgets(version,sizeof(version),f)||!fgets(name,sizeof(name),f)
      ||fscanf(f,"%d %d %d",&players,&max,&pw)!=3){fclose(f);return 0;}
  fclose(f);
  magic[strcspn(magic,"\r\n")]=0;endpoint[strcspn(endpoint,"\r\n")]=0;game[strcspn(game,"\r\n")]=0;
  version[strcspn(version,"\r\n")]=0;name[strcspn(name,"\r\n")]=0;
  if(strcmp(magic,"RNET_LAN_ROOM_4")||strcmp(game,game_name())||strcmp(version,game_version())||max<2||max>4||players<1||players>max)return 0;
  memset(out,0,sizeof(*out));room4_id(out->lobby_id,sizeof(out->lobby_id),endpoint);
  snprintf(out->name,sizeof(out->name),"LAN - %.57s",name);snprintf(out->game_name,sizeof(out->game_name),"%s",game);
  snprintf(out->game_version,sizeof(out->game_version),"%s",version);out->player_count=players;out->max_slots=max;
  out->has_password=pw!=0;out->latency_ms=-1;return 1;
}
static void room4_publish(void) {
  const RNetLanRoom* room=room4_state();RNetLanBeaconRoom beacon;FILE* f;
  if(!r4_host || !r4_peer || room->starting)return;
  if(room->revision!=r4_published) {
    f=fopen(lan_path(),"wb");
    if(f){fprintf(f,"RNET_LAN_ROOM_4\n%s\n%s\n%s\n%s\n%d %d %d\n",room->endpoint,room->game,room->game_version,
      room->name,room4_count(room),room->max_slots,r4_password[0]!=0);fclose(f);r4_published=room->revision;}
  }
  if(!r4_beacon && rnet_lan_beacon_publish_open(&r4_beacon,0))return;
  memset(&beacon,0,sizeof(beacon));room4_id(beacon.lobby_id,sizeof(beacon.lobby_id),room->endpoint);
  snprintf(beacon.endpoint,sizeof(beacon.endpoint),"%s",room->endpoint);snprintf(beacon.game_name,sizeof(beacon.game_name),"%s",room->game);
  snprintf(beacon.game_version,sizeof(beacon.game_version),"%s",room->game_version);snprintf(beacon.room_name,sizeof(beacon.room_name),"%.63s",room->name);
  beacon.player_count=room4_count(room);beacon.max_slots=room->max_slots;beacon.has_password=r4_password[0]!=0;
  if(!rnet_lan_beacon_publish_set_room(r4_beacon,&beacon))rnet_lan_beacon_publish_tick(r4_beacon);
}
static void room4_unpublish(void) {
  RecompLauncherCNetplayLobby row;char id[40];room4_id(id,sizeof(id),r4_endpoint);
  if(r4_host && room4_index(&row) && !strcmp(row.lobby_id,id))remove(lan_path());
  rnet_lan_beacon_close(&r4_beacon);r4_published=0;
}
static int room4_leave(void) {
  room4_unpublish();rnet_lan_room_close(&r4_peer);memset(&r4_last,0,sizeof(r4_last));
  memset(&r4_launch,0,sizeof(r4_launch));r4_active=r4_host=r4_reopen=0;r4_endpoint[0]=r4_bind[0]=0;
  r4_chat_head=r4_chat_count=r4_chat_sequence=0;
  memset(r4_password,0,sizeof(r4_password));return 0;
}
static int room4_open(void) {
  const char* name=cb_player_name(NULL);int result;
  if(r4_host)result=rnet_lan_room_host_open(&r4_peer,r4_bind,&r4_last,r4_password);
  else result=rnet_lan_room_guest_open(&r4_peer,r4_bind,r4_endpoint,game_name(),game_version(),r4_password,
      name&&name[0]?name:"Player",g_h.lan_max_players);
  if(result)room4_error(result);return result;
}
static void room4_rematch(void) {
  rnet_lan_room_close(&r4_peer);memset(&r4_launch,0,sizeof(r4_launch));r4_reopen=1;
  r4_last.starting=0;r4_last.session_id=0;r4_last.ready_mask=0;r4_published=0;
  r4_chat_head=r4_chat_count=r4_chat_sequence=0;
}
static int room4_create(void* ctx,const char* name,char* endpoint,const char* password,
                         const RecompLauncherCSettings* settings,int lan,int slots) {
  RNetLobbyMatchCaps caps;const char* player;const char* colon;
  if(!lan)return cb_create(ctx,name,endpoint,password,settings,lan,slots);
  recomp_netplay_host_leave();memset(&r4_last,0,sizeof(r4_last));caps=default_caps(settings);player=cb_player_name(NULL);
  if(slots<2)slots=2;if(slots>g_h.lan_max_players)slots=g_h.lan_max_players;
  snprintf(r4_last.name,sizeof(r4_last.name),"%s",name&&name[0]?name:"Lobby");
  snprintf(r4_last.game,sizeof(r4_last.game),"%s",game_name());snprintf(r4_last.game_version,sizeof(r4_last.game_version),"%s",game_version());
  snprintf(r4_endpoint,sizeof(r4_endpoint),"%s",endpoint);snprintf(r4_last.endpoint,sizeof(r4_last.endpoint),"%s",endpoint);
  snprintf(r4_password,sizeof(r4_password),"%s",password?password:"");
  snprintf(r4_last.member[0].name,sizeof(r4_last.member[0].name),"%s",player&&player[0]?player:"Host");
  r4_last.max_slots=slots;r4_last.input_delay=caps.input_delay;r4_last.rollback=caps.rollback;
  r4_last.input_prediction=caps.input_prediction>=2?caps.input_prediction:4+caps.input_delay;
  if(r4_last.input_prediction>16)r4_last.input_prediction=16;
  r4_last.session_variant=caps.session_variant;snprintf(r4_last.mod_set,sizeof(r4_last.mod_set),"%s",caps.mod_set);
  colon=strrchr(endpoint,':');if(!colon)return -1;snprintf(r4_bind,sizeof(r4_bind),"0.0.0.0:%s",colon+1);
  r4_host=r4_active=1;if(room4_open()){room4_leave();return -4;}
  snprintf(g_resume_endpoint,sizeof(g_resume_endpoint),"%s",r4_endpoint);room4_publish();return 0;
}
static int room4_join(void* ctx,const char* id,const char* password,char* bind) {
  const char* endpoint;
  if(!strncmp(id,"lr4:",4))endpoint=id+4;
  else if(!strncmp(id,"lan:",4))endpoint=id+4;
  else return cb_join(ctx,id,password,bind);
  recomp_netplay_host_leave();snprintf(r4_endpoint,sizeof(r4_endpoint),"%s",endpoint);
  snprintf(r4_bind,sizeof(r4_bind),"%s",bind&&bind[0]?bind:"0.0.0.0:0");
  snprintf(r4_password,sizeof(r4_password),"%s",password?password:"");r4_active=1;r4_host=0;
  if(room4_open()){room4_leave();return -1;}
  snprintf(g_resume_endpoint,sizeof(g_resume_endpoint),"%s",r4_endpoint);return 0;
}
static void room4_pump(void* ctx) {
  int status;cb_pump(ctx);if(!r4_active)return;
  if(r4_reopen && !r4_peer){if(!room4_open())r4_reopen=0;else return;}
  if(!r4_peer)return;status=rnet_lan_room_pump(r4_peer);
  if(status<0){room4_error(status);return;}
  r4_last=*rnet_lan_room_state(r4_peer);room4_publish();
  {
    RNetLanChatLine line;int local=rnet_lan_room_local_slot(r4_peer);char local_id[64];
    snprintf(local_id,sizeof(local_id),"lan:%08x",local>=0?(unsigned)r4_last.member[local].id:0);
    while(rnet_lan_room_take_chat(r4_peer,&line)) {
      unsigned index;RecompLauncherCNetplayChatMessage* message;
      if(r4_chat_count<64){index=(r4_chat_head+r4_chat_count)%64;++r4_chat_count;}
      else {index=r4_chat_head;r4_chat_head=(r4_chat_head+1)%64;}
      message=&r4_chat[index];memset(message,0,sizeof(*message));
      snprintf(message->from,sizeof(message->from),"%s",line.from);rnet_chat_filter_copy(line.text,message->text,sizeof(message->text));
      message->is_local=!strcmp(local_id,line.player_id);message->seq=++r4_chat_sequence;
    }
  }
}
static int room4_list_count(void* ctx) {
  RecompLauncherCNetplayLobby index,row;int i,n=cb_list_count(ctx);
  if(room4_index(&index)) {
    for(i=0;i<n;++i)if(cb_list_get(ctx,i,&row)&&!strcmp(row.lobby_id,index.lobby_id))return n;
    return n+1;
  }
  return n;
}
static int room4_list_get(void* ctx,int index,RecompLauncherCNetplayLobby* out) {
  int n=cb_list_count(ctx);if(index<n)return cb_list_get(ctx,index,out);
  return index==n && room4_list_count(ctx)>n && room4_index(out);
}
static int room4_in_lobby(void* ctx){return r4_active?1:cb_in_lobby(ctx);}
static int room4_is_host(void* ctx){return r4_active?r4_host:cb_is_host(ctx);}
static int room4_member_count(void* ctx){return r4_active?room4_count(room4_state()):cb_member_count(ctx);}
static int room4_member_get(void* ctx,int index,RecompLauncherCNetplayMember* out) {
  const RNetLanRoom* room;int i,local;
  if(!r4_active)return cb_member_get(ctx,index,out);if(!out||index<0)return 0;room=room4_state();
  local=r4_peer?rnet_lan_room_local_slot(r4_peer):-1;
  for(i=0;i<room->max_slots;++i)if(room->member[i].id && index--==0) {
    memset(out,0,sizeof(*out));out->slot=i;out->ready=(room->ready_mask&(1u<<i))!=0;
    out->is_host=i==room->host_slot;out->is_local=i==local;out->latency_ms=rnet_lan_room_member_rtt(r4_peer,i);
    snprintf(out->display_name,sizeof(out->display_name),"%s",room->member[i].name);return 1;
  }
  return 0;
}
static int room4_move(void* ctx,int from,int to){return r4_active?rnet_lan_room_move(r4_peer,from,to):cb_move_member(ctx,from,to);}
static int room4_move_self(void* ctx,int to){return r4_active?rnet_lan_room_move(r4_peer,rnet_lan_room_local_slot(r4_peer),to):cb_seat_move_self(ctx,to);}
static int room4_kick(void* ctx,int slot){return r4_active?rnet_lan_room_kick(r4_peer,slot):cb_kick_member(ctx,slot);}
static int room4_local_ready(void* ctx) {
  int local;if(!r4_active)return cb_local_ready(ctx);local=rnet_lan_room_local_slot(r4_peer);
  return local>=0 && (room4_state()->ready_mask&(1u<<local))!=0;
}
static int room4_all_ready(void* ctx){return r4_active?rnet_lan_room_all_ready(r4_peer):cb_all_ready(ctx);}
static int room4_ready(void* ctx,int ready){return r4_active?rnet_lan_room_set_ready(r4_peer,ready):cb_set_ready(ctx,ready);}
static int room4_start(void* ctx,const RecompLauncherCSettings* settings) {
  (void)settings;return r4_active?rnet_lan_room_start(r4_peer):cb_request_start(ctx,settings);
}
static int room4_launch_pending(void* ctx) {
  if(!r4_active)return cb_launch_pending(ctx);room4_pump(ctx);
  return r4_launch.enabled || rnet_lan_room_launch_ready(r4_peer);
}
static void room4_clear_launch(void* ctx){if(!r4_active)cb_clear_launch_pending(ctx);}
static int room4_fill_launch(void* ctx,RecompLauncherCNetplayLaunch* out) {
  const RNetLanRoom* room;int seats[4],count=0,i,local;SlotPlan plan;
  if(!r4_active)return cb_fill_launch(ctx,out);if(!out)return 0;
  if(r4_launch.enabled){*out=r4_launch;return 1;}
  if(!rnet_lan_room_launch_ready(r4_peer))return 0;
  room=room4_state();if(!supports_session_variant(room->session_variant))return 0;
  local=rnet_lan_room_local_slot(r4_peer);if(local<0)return 0;
  for(i=0;i<room->max_slots;++i)if(room->member[i].id)seats[count++]=i;
  plan_session_slots(seats,count,room->host_slot,local,r4_host,&plan);
  memset(&r4_launch,0,sizeof(r4_launch));r4_launch.enabled=1;r4_launch.local_slot=plan.local_slot;
  launch_apply_plan(&r4_launch,&plan);r4_launch.player_count=count;r4_launch.max_slots=room->max_slots;
  r4_launch.input_player=g_h.input_player;r4_launch.input_delay=room->input_delay;r4_launch.rollback=room->rollback;
  r4_launch.input_prediction=room->input_prediction;r4_launch.session_id=room->session_id;r4_launch.session_variant=room->session_variant;
  r4_launch.transport_host=1;snprintf(r4_launch.bind_hostport,sizeof(r4_launch.bind_hostport),"%s",r4_bind);
  if(!r4_host)snprintf(r4_launch.peer_hostport,sizeof(r4_launch.peer_hostport),"%s",r4_endpoint);
  r4_last=*room;room4_unpublish();rnet_lan_room_close(&r4_peer);*out=r4_launch;return 1;
}
static int room4_max_slots(void* ctx){return r4_active?room4_state()->max_slots:cb_lobby_max_slots(ctx);}
static int room4_create_max(void* ctx,int lan){return lan?g_h.lan_max_players:cb_create_max_slots(ctx,lan);}
static int room4_caps(int delay,int rollback,int prediction,int variant) {
  return rnet_lan_room_set_caps(r4_peer,delay,rollback,prediction,variant,room4_state()->mod_set);
}
static int room4_delay_get(void* ctx){return r4_active?room4_state()->input_delay:cb_input_delay_get(ctx);}
static int room4_delay_set(void* ctx,int value){const RNetLanRoom* r=room4_state();return r4_active?room4_caps(clamp_input_delay(value),r->rollback,r->input_prediction,r->session_variant):cb_input_delay_set(ctx,value);}
static int room4_prediction_get(void* ctx){return r4_active?room4_state()->input_prediction:cb_input_prediction_get(ctx);}
static int room4_prediction_set(void* ctx,int value){const RNetLanRoom* r=room4_state();return r4_active?room4_caps(r->input_delay,r->rollback,clamp_input_prediction(value),r->session_variant):cb_input_prediction_set(ctx,value);}
static int room4_rollback_get(void* ctx){return r4_active?room4_state()->rollback:cb_rollback_get(ctx);}
static int room4_rollback_set(void* ctx,int value){const RNetLanRoom* r=room4_state();return r4_active?room4_caps(r->input_delay,value!=0,r->input_prediction,r->session_variant):cb_rollback_set(ctx,value);}
static int room4_variant_get(void* ctx){return r4_active?room4_state()->session_variant:cb_session_variant_get(ctx);}
static int room4_variant_set(void* ctx,int value){const RNetLanRoom* r=room4_state();return r4_active && supports_session_variant(value)?room4_caps(r->input_delay,r->rollback,r->input_prediction,value):cb_session_variant_set(ctx,value);}
static int room4_no_gallery(void* ctx){return r4_active?0:cb_lobby_allow_spectators(ctx);}
static int room4_no_host_gallery(void* ctx){return !r4_active && g_callbacks.host_can_spectate?g_callbacks.host_can_spectate(ctx):0;}
static int room4_chat_count(void* ctx){return r4_active?(int)r4_chat_count:cb_chat_count(ctx);}
static int room4_chat_send(void* ctx,const char* text){return r4_active?rnet_lan_room_send_chat(r4_peer,text):cb_chat_send(ctx,text);}
static int room4_chat_get(void* ctx,int index,RecompLauncherCNetplayChatMessage* out) {
  if(!r4_active)return cb_chat_get(ctx,index,out);
  if(!out || index<0 || (unsigned)index>=r4_chat_count)return 0;
  *out=r4_chat[(r4_chat_head+(unsigned)index)%64];return 1;
}
static int room4_swap_request(void* ctx,int slot){return r4_active?-1:cb_seat_swap_request(ctx,slot);}
static int room4_swap_incoming(void* ctx,char* who,size_t cap,int* slot){return r4_active?0:cb_seat_swap_incoming(ctx,who,cap,slot);}
static int room4_swap_respond(void* ctx,int accept){return r4_active?-1:cb_seat_swap_respond(ctx,accept);}
static int room4_swap_outgoing(void* ctx){return r4_active?0:cb_seat_swap_outgoing(ctx);}
static void room4_swap_clear(void* ctx){if(!r4_active)cb_seat_swap_clear(ctx);}
static const RecompLauncherCNetplayCallbacks* room4_callbacks(void) {
  static RecompLauncherCNetplayCallbacks callbacks;
  callbacks=g_callbacks;callbacks.create=room4_create;callbacks.join=room4_join;callbacks.pump=room4_pump;
  callbacks.list_count=room4_list_count;callbacks.list_get=room4_list_get;callbacks.in_lobby=room4_in_lobby;callbacks.is_host=room4_is_host;
  callbacks.member_count=room4_member_count;callbacks.member_get=room4_member_get;callbacks.move_member=room4_move;callbacks.seat_move_self=room4_move_self;
  callbacks.kick_member=room4_kick;callbacks.local_ready=room4_local_ready;callbacks.all_ready=room4_all_ready;callbacks.set_ready=room4_ready;
  callbacks.request_start=room4_start;callbacks.launch_pending=room4_launch_pending;callbacks.clear_launch_pending=room4_clear_launch;
  callbacks.fill_launch=room4_fill_launch;callbacks.lobby_max_slots=room4_max_slots;callbacks.create_max_slots=room4_create_max;
  callbacks.input_delay_get=room4_delay_get;callbacks.input_delay_set=room4_delay_set;
  callbacks.input_prediction_get=room4_prediction_get;callbacks.input_prediction_set=room4_prediction_set;
  callbacks.rollback_get=room4_rollback_get;callbacks.rollback_set=room4_rollback_set;
  callbacks.session_variant_get=room4_variant_get;callbacks.session_variant_set=room4_variant_set;
  callbacks.lobby_allow_spectators=room4_no_gallery;callbacks.host_can_spectate=room4_no_host_gallery;
  callbacks.chat_count=room4_chat_count;callbacks.chat_send=room4_chat_send;callbacks.chat_get=room4_chat_get;
  /* The wire currently has no consent exchange for occupied-seat trades. */
  callbacks.seat_swap_request=room4_swap_request;callbacks.seat_swap_incoming=room4_swap_incoming;callbacks.seat_swap_respond=room4_swap_respond;
  callbacks.seat_swap_outgoing=room4_swap_outgoing;callbacks.seat_swap_clear=room4_swap_clear;return &callbacks;
}
