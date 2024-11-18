#include <chain/taiyi_fwd.hpp>

#include <chain/taiyi_evaluator.hpp>
#include <chain/database.hpp>
#include <chain/taiyi_objects.hpp>
#include <chain/block_summary_object.hpp>
#include <chain/account_object.hpp>
#include <chain/tiandao_property_object.hpp>
#include <chain/nfa_objects.hpp>
#include <chain/actor_objects.hpp>
#include <chain/zone_objects.hpp>

#include <chain/lua_context.hpp>
#include <chain/contract_worker.hpp>

#include <chain/util/manabar.hpp>

#include <fc/macros.hpp>

#include <limits>

extern std::wstring utf8_to_wstring(const std::string& str);
extern std::string wstring_to_utf8(const std::wstring& str);

namespace taiyi { namespace chain {

    extern std::string s_debug_actor;

    std::string g_zone_type_strings[_ZONE_TYPE_NUM] = {
        "YUANYE",         //原野
        "HUPO",           //湖泊
        "NONGTIAN",       //农田

        "LINDI",          //林地
        "MILIN",          //密林
        "YUANLIN",        //园林
        
        "SHANYUE",        //山岳
        "DONGXUE",        //洞穴
        "SHILIN",         //石林
        
        "QIULIN",         //丘陵
        "TAOYUAN",        //桃源
        "SANGYUAN",       //桑园
        
        "XIAGU",          //峡谷
        "ZAOZE",          //沼泽
        "YAOYUAN",        //药园
        
        "HAIYANG",        //海洋
        "SHAMO",          //沙漠
        "HUANGYE",        //荒野
        "ANYUAN",         //暗渊

        "DUHUI",          //都会
        "MENPAI",         //门派
        "SHIZHEN",        //市镇
        "GUANSAI",        //关寨
        "CUNZHUANG"      //村庄
    };

    static E_ZONE_TYPE get_zone_type_from_string(const std::string& sType) {
        for(unsigned int i=0; i<_ZONE_TYPE_NUM; i++) {
            if(sType == g_zone_type_strings[i])
                return (E_ZONE_TYPE)i;
        }
        
        return _ZONE_INVALID_TYPE;
    }
    
    static string get_zone_type_string(E_ZONE_TYPE type) {
        if(type >= _ZONE_TYPE_NUM)
            return "";
        else
            return g_zone_type_strings[type];
    }
    //=============================================================================
    operation_result create_zone_evaluator::do_apply( const create_zone_operation& o )
    { try {
        const auto& creator = _db.get_account( o.creator ); // prove it exists
        auto now = _db.head_block_time();
        
        //when creator is not sifu, must put proposal in post for voting
//        if(o.creator != TAIYI_COMMITTEE_ACCOUNT) {
//            FC_ASSERT( ( now - auth.last_root_post ) > TAIYI_MIN_ROOT_COMMENT_INTERVAL, "You may only post once every 5 minutes.", ("now",now)("last_root_post", auth.last_root_post) );
//        }
            
        //check zone existence
        auto check_zone = _db.find< zone_object, by_name >( o.name );
        FC_ASSERT( check_zone == nullptr, "There is already exist zone named \"${a}\".", ("a", o.name) );

        //check type
        E_ZONE_TYPE zone_type = get_zone_type_from_string(o.type);
        FC_ASSERT(zone_type != _ZONE_INVALID_TYPE, "zone type \"${t}\" is not valid.", ("t", o.type));

//        char permlink[TAIYI_MAX_PERMLINK_LENGTH];
//        sprintf(permlink, "--zone-creation-proposal--%u", o.uid);
//        validate_permlink_0_1( permlink );
//
//        char title[TAIYI_COMMENT_TITLE_LIMIT];
//        sprintf(title, "[Zone]%s", o.name.c_str());
//
//        const auto& by_permlink_idx = _db.get_index< comment_index >().indices().get< by_permlink >();
//        auto itr = by_permlink_idx.find( boost::make_tuple( o.creator, permlink ) );
//        FC_ASSERT(itr == by_permlink_idx.end(), "Can not edit an exist proposal! permlink=${l}", ("l", permlink));
//
//        _db.modify( auth, [&]( account_object& a ) {
//            a.last_root_post = now;
//            a.last_post = now;
//            a.last_post_edit = now;
//            a.post_count++;
//        });
//        
//        const auto& new_comment = _db.create< comment_object >( [&]( comment_object& com ) {
//            com.type = comment_types::CT_ZONE_CREATION;
//            com.rule_uuid = 0;
//            com.author = o.creator;
//            com.permlink = permlink;
//            com.last_update = _db.head_block_time();
//            com.created = com.last_update;
//            com.active = com.last_update;
//            com.max_cashout_time = fc::time_point_sec::maximum();
//            com.reward_weight = TAIYI_100_PERCENT;
//            
//            com.parent_author = "";
//            com.parent_permlink = "zone-creation";
//            com.category = "ZONE-CREATION";
//            com.root_comment = com.id;
//            
//            if(o.creator == TAIYI_COMMITTEE_ACCOUNT) {
//                com.last_payout = now;
//                com.cashout_time = fc::time_point_sec::maximum();
//            }
//            else {
//                com.last_payout = fc::time_point_sec::min();
//                com.cashout_time = com.created + TAIYI_CASHOUT_WINDOW_SECONDS;
//            }
//        });
//
//        _db.create< comment_content_object >( [&]( comment_content_object& con ) {
//            con.comment = new_comment.id;
//            
//            con.title = title;
//
//            char body[1024];
//            sprintf(body, "This is a proposal to create new zone named \"%s\" with type=\"%s\"", o.name.c_str(), o.type.c_str());
//            con.body = body;
//            
//            zone_creation_data data = {o.name, zone_type};
//            string json_metadata = fc::json::to_string( data );
//            con.json_metadata = json_metadata;
//        });

        const auto& props = _db.get_dynamic_global_properties();
        //const siming_schedule_object& wso = _db.get_siming_schedule_object();

        contract_result result;

        //creator is sifu, approve this proposal immediately
        if(o.creator == TAIYI_COMMITTEE_ACCOUNT) {
            //先创建NFA
            string nfa_symbol_name = "nfa.zone.default";
            const auto* nfa_symbol = _db.find<nfa_symbol_object, by_symbol>(nfa_symbol_name);
            FC_ASSERT(nfa_symbol != nullptr, "NFA symbol named \"${n}\" is not exist.", ("n", nfa_symbol_name));
            
            const auto* current_trx = _db.get_current_trx_ptr();
            FC_ASSERT(current_trx);
            const flat_set<public_key_type>& sigkeys = current_trx->get_signature_keys(_db.get_chain_id(), fc::ecc::fc_canonical);
            
            LuaContext context;
            _db.initialize_VM_baseENV(context);
            
            const auto& nfa = _db.create_nfa(creator, *nfa_symbol, sigkeys, true, context);
            
            protocol::nfa_affected affected;
            affected.affected_account = creator.name;
            affected.affected_item = nfa.id;
            affected.action = nfa_affected_type::create_for;
            result.contract_affecteds.push_back(std::move(affected));
            
            affected.affected_account = creator.name;
            affected.action = nfa_affected_type::create_by;
            result.contract_affecteds.push_back(std::move(affected));
            
            const auto& new_zone = _db.create< zone_object >( [&]( zone_object& zone ) {
                _db.initialize_zone_object( zone, o.name, nfa, zone_type);
            });
                        
            _db.grow_zone(new_zone);
            
            //_db.push_virtual_operation( zone_creation_approved_operation( new_comment.author, new_comment.permlink, new_zone.name ) );
        }
        
        return result;
    } FC_CAPTURE_AND_RETHROW( (o) ) }
    //=============================================================================
    void get_connected_zones(const zone_object& zone, std::set<zone_id_type>& connected_zones, database& db)
    {
        const auto& connect_by_from_idx = db.get_index< zone_connect_index, by_zone_from >();
        auto itrf = connect_by_from_idx.lower_bound( zone.id );
        auto endf = connect_by_from_idx.end();
        while( itrf != endf )
        {
            if(itrf->from != zone.id)
                break;
            if(connected_zones.find(itrf->to) == connected_zones.end())
                connected_zones.insert(itrf->to);
            ++itrf;
        }
        const auto& connect_by_to_idx = db.get_index< zone_connect_index, by_zone_to >();
        auto itrt = connect_by_to_idx.lower_bound( zone.id );
        auto endt = connect_by_to_idx.end();
        while( itrt != endt )
        {
            if(itrt->to != zone.id)
                break;
            if(connected_zones.find(itrt->from) == connected_zones.end())
                connected_zones.insert(itrt->from);
            ++itrt;
        }
    }

    operation_result connect_to_zone_evaluator::do_apply( const connect_to_zone_operation& o )
    { try {
        const auto& auth = _db.get_account( o.account ); // prove it exists
        FC_UNUSED(auth);

        const auto* to_zone = _db.find<zone_object, by_name>(o.to);
        FC_ASSERT( to_zone != nullptr, "There is no zone named \"${a}\".", ("a", o.to) );
        const auto& to_zone_nfa = _db.get<nfa_object, by_id>(to_zone->nfa_id);
        const auto& owner = _db.get<account_object, by_id>(to_zone_nfa.owner_account);
        FC_ASSERT( owner.name == o.account, "account ${a} is not the owner of zone ${z}", ("a", o.account)("z", o.to) );

        const auto* from_zone = _db.find< zone_object, by_name >( o.from );
        FC_ASSERT( from_zone != nullptr, "There is no zone named \"${a}\".", ("a", o.from) );
        
        const auto* check_connect = _db.find<zone_connect_object, by_zone_from>( boost::make_tuple(from_zone->id, to_zone->id) );
        FC_ASSERT( check_connect == nullptr, "Connection from \"${a}\" to \"${b}\" is already exist.", ("a", from_zone->name)("b", to_zone->name) );

        //auto now = _db.head_block_time();
        //const auto& props = _db.get_dynamic_global_properties();
        //const siming_schedule_object& wso = _db.get_siming_schedule_object();
        
        const auto& tiandao = _db.get_tiandao_properties();

        //检查连接区域是否达到上限
        std::set<zone_id_type> connected_zones;
        uint max_num = tiandao.zone_type_connection_max_num_map.at(from_zone->type);
        get_connected_zones(*from_zone, connected_zones, _db);
        FC_ASSERT(connected_zones.size() < max_num || connected_zones.find(to_zone->id) != connected_zones.end(), "The \"from zone\"'s connections exceed the limit.");
        connected_zones.clear();
        max_num = tiandao.zone_type_connection_max_num_map.at(to_zone->type);
        FC_ASSERT(connected_zones.size() < max_num || connected_zones.find(from_zone->id) != connected_zones.end(), "The \"to zone\"'s connections exceed the limit.");

        //create connection
        _db.create< zone_connect_object >( [&]( zone_connect_object& o ) {
            o.from = from_zone->id;
            o.to = to_zone->id;
        });
        
        return void_result();
    } FC_CAPTURE_AND_RETHROW( (o) ) }

} } // taiyi::chain

