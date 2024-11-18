#include <chain/taiyi_fwd.hpp>

#include <protocol/taiyi_operations.hpp>

#include <chain/database.hpp>
#include <chain/database_exceptions.hpp>
#include <chain/global_property_object.hpp>
#include <chain/taiyi_objects.hpp>
#include <chain/transaction_object.hpp>
#include <chain/account_object.hpp>
#include <chain/contract_objects.hpp>
#include <chain/nfa_objects.hpp>
#include <chain/asset_objects/nfa_balance_object.hpp>

#include <chain/lua_context.hpp>
#include <chain/contract_worker.hpp>

#include <chain/util/uint256.hpp>
#include <chain/util/manabar.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>

#include <fc/container/deque.hpp>

#include <fc/io/fstream.hpp>

#include <iostream>

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>


namespace taiyi { namespace chain {

    void database::create_basic_nfa_symbol_objects()
    {
        const auto& creator = get_account( TAIYI_YEMING_ACCOUNT );
        create_nfa_symbol_object(creator, "nfa.actor.default", "默认的角色", "contract.actor.default");
        create_nfa_symbol_object(creator, "nfa.zone.default", "默认的区域", "contract.zone.default");
    }
    //=========================================================================
    size_t database::create_nfa_symbol_object(const account_object& creator, const string& symbol, const string& describe, const string& default_contract)
    {
        const auto* nfa_symbol = find<nfa_symbol_object, by_symbol>(symbol);
        FC_ASSERT(nfa_symbol == nullptr, "NFA symbol named \"${n}\" is already exist.", ("n", symbol));
        
        const auto& contract = get<contract_object, by_name>(default_contract);
        auto abi_itr = contract.contract_ABI.find(lua_types(lua_string(TAIYI_NFA_INIT_FUNC_NAME)));
        FC_ASSERT(abi_itr != contract.contract_ABI.end(), "contract ${c} has not init function named ${i}", ("c", contract.name)("i", TAIYI_NFA_INIT_FUNC_NAME));
        
        const auto& nfa_symbol_obj = create<nfa_symbol_object>([&](nfa_symbol_object& obj) {
            obj.creator = creator.name;
            obj.symbol = symbol;
            obj.describe = describe;
            obj.default_contract = contract.id;
            obj.count = 0;
        });
        
        return fc::raw::pack_size(nfa_symbol_obj);
    }
    //=========================================================================
    const nfa_object& database::create_nfa(const account_object& creator, const nfa_symbol_object& nfa_symbol, const flat_set<public_key_type>& sigkeys, bool reset_vm_memused, LuaContext& context)
    {
        const auto& caller = creator;
        modify( caller, [&]( account_object& a ) {
            util::update_manabar( get_dynamic_global_properties(), a, true );
        });
        
        const auto& nfa = create<nfa_object>([&](nfa_object& obj) {
            obj.creator_account = creator.id;
            obj.owner_account = creator.id;
            
            obj.symbol_id = nfa_symbol.id;
            obj.main_contract = nfa_symbol.default_contract;
            
            obj.created_time = head_block_time();
        });
        
        //运行主合约初始化nfa数据
        const auto& contract = get<contract_object, by_id>(nfa.main_contract);
        
        //evaluate contract authority
        if (contract.check_contract_authority)
        {
            auto skip = node_properties().skip_flags;
            if (!(skip & (database::validation_steps::skip_transaction_signatures | database::validation_steps::skip_authority_check)))
            {
                auto key_itr = std::find(sigkeys.begin(), sigkeys.end(), contract.contract_authority);
                FC_ASSERT(key_itr != sigkeys.end(), "No contract related permissions were found in the signature, contract_authority:${c}", ("c", contract.contract_authority));
            }
        }
        
        const auto* op_acd = find<account_contract_data_object, by_account_contract>(boost::make_tuple(caller.id, contract.id));
        if(op_acd == nullptr) {
            create<account_contract_data_object>([&](account_contract_data_object &a) {
                a.owner = caller.id;
                a.contract_id = contract.id;
            });
            op_acd = find<account_contract_data_object, by_account_contract>(boost::make_tuple(caller.id, contract.id));
        }
        lua_map account_data = op_acd->contract_data;
        
        contract_result result;
        contract_worker worker;
        vector<lua_types> value_list;
        
        //mana可能在执行合约中被进一步使用，所以这里记录当前的mana来计算虚拟机的执行消耗
        long long old_drops = caller.manabar.current_mana / TAIYI_USEMANA_EXECUTION_SCALE;
        long long vm_drops = old_drops;
        lua_table result_table = worker.do_contract_function_return_table(caller, TAIYI_NFA_INIT_FUNC_NAME, value_list, account_data, sigkeys, result, contract, vm_drops, reset_vm_memused, context, *this);
        int64_t used_drops = old_drops - vm_drops;
        
        size_t new_state_size = fc::raw::pack_size(nfa);
        int64_t used_mana = used_drops * TAIYI_USEMANA_EXECUTION_SCALE + new_state_size * TAIYI_USEMANA_STATE_BYTES_SCALE + 100 * TAIYI_USEMANA_EXECUTION_SCALE;
        FC_ASSERT( caller.manabar.has_mana(used_mana), "Creator account does not have enough mana to create nfa." );
        modify( caller, [&]( account_object& a ) {
            a.manabar.use_mana( used_mana );
        });
        
        //reward contract owner
        const auto& contract_owner = get<account_object, by_id>(contract.owner);
        reward_contract_owner(contract_owner.name, asset(used_mana, QI_SYMBOL));
        
        uint64_t contract_private_data_size    = 3L * 1024;
        uint64_t contract_total_data_size      = 10L * 1024 * 1024;
        uint64_t contract_max_data_size        = 2L * 1024 * 1024 * 1024;
        FC_ASSERT(fc::raw::pack_size(account_data) <= contract_private_data_size, "the contract private data size is too large.");
        FC_ASSERT(fc::raw::pack_size(contract.contract_data) <= contract_total_data_size, "the contract total data size is too large.");
        
        modify(*op_acd, [&](account_contract_data_object &a) {
            a.contract_data = account_data;
        });
        
        //init nfa from result table
        modify(nfa, [&](nfa_object& obj) {
            obj.data = result_table.v;
        });
        
        return nfa;
    }
    //=============================================================================
    void database::process_nfa_tick()
    {
        auto now = head_block_time();

        //list NFAs this tick will sim
        const auto& nfa_idx = get_index< nfa_index, by_tick_time >();
        auto run_num = nfa_idx.size() / TAIYI_NFA_TICK_PERIOD_MAX_BLOCK_NUM + 1;
        auto itnfa = nfa_idx.begin();
        auto endnfa = nfa_idx.end();
        std::vector<const nfa_object*> tick_nfas;
        tick_nfas.reserve(run_num);
        while( itnfa != endnfa && run_num > 0  )
        {
            if(itnfa->next_tick_time > now)
                break;
            tick_nfas.push_back(&(*itnfa));
            ++itnfa;
            run_num--;
        }
        
        for(const auto* n : tick_nfas) {
            const auto& nfa = *n;
            
            const auto* contract_ptr = find<contract_object, by_id>(nfa.main_contract);
            if(contract_ptr == nullptr)
                continue;
            auto abi_itr = contract_ptr->contract_ABI.find(lua_types(lua_string("heart_beat")));
            if(abi_itr == contract_ptr->contract_ABI.end()) {
                modify(nfa, [&]( nfa_object& obj ) { obj.next_tick_time = time_point_sec::maximum(); }); //disable tick
                continue;
            }

            modify(nfa, [&]( nfa_object& obj ) {
                obj.next_tick_time = now + TAIYI_NFA_TICK_PERIOD_MAX_BLOCK_NUM * TAIYI_BLOCK_INTERVAL;
                util::update_manabar( get_dynamic_global_properties(), obj, true );
            });

            vector<lua_types> value_list; //no params.
            contract_result cresult;
            contract_worker worker;

            LuaContext context;
            initialize_VM_baseENV(context);
            
            //mana可能在执行合约中被进一步使用，所以这里记录当前的mana来计算虚拟机的执行消耗
            long long old_drops = nfa.manabar.current_mana / TAIYI_USEMANA_EXECUTION_SCALE;
            long long vm_drops = old_drops;
            bool beat_fail = false;
            try {
                worker.do_nfa_contract_action(nfa, "heart_beat", value_list, cresult, vm_drops, true, context, *this);
            }
            catch (fc::exception e) {
                //任何错误都不能照成核心循环崩溃
                beat_fail = true;
                wlog("NFA (${i}) process heart beat fail. err: ${e}", ("i", nfa.id)("e", e.to_string()));
            }
            catch (...) {
                //任何错误都不能照成核心循环崩溃
                beat_fail = true;
                wlog("NFA (${i}) process heart beat fail.", ("i", nfa.id));
            }
            int64_t used_drops = old_drops - vm_drops;

            int64_t used_mana = used_drops * TAIYI_USEMANA_EXECUTION_SCALE + 50 * TAIYI_USEMANA_EXECUTION_SCALE;
            modify( nfa, [&]( nfa_object& obj ) {
                if( obj.manabar.current_mana < used_mana )
                    obj.manabar.current_mana = 0;
                else
                    obj.manabar.current_mana -= used_mana;
                //执行错误不仅要扣费，还会将NFA重置为关闭心跳状态
                if(beat_fail)
                    obj.next_tick_time = time_point_sec::maximum();
            });
            
            //reward contract owner
            const auto& contract_owner = get<account_object, by_id>(contract_ptr->owner);
            reward_contract_owner(contract_owner.name, asset(used_mana, QI_SYMBOL));
        }
    }
    //=========================================================================
    asset database::get_nfa_balance( const nfa_object& nfa, asset_symbol_type symbol ) const
    {
        if(symbol.asset_num == TAIYI_ASSET_NUM_QI) {
            return nfa.qi;
        }
        else {
            auto key = boost::make_tuple( nfa.id, symbol );
            const nfa_regular_balance_object* rbo = find< nfa_regular_balance_object, by_nfa_liquid_symbol >( key );
            if( rbo == nullptr )
            {
                return asset(0, symbol);
            }
            else
            {
                return rbo->liquid;
            }
        }
    }
    //=============================================================================
    template< typename nfa_balance_object_type, class balance_operator_type >
    void database::adjust_nfa_balance( const nfa_id_type& nfa_id, const asset& delta, balance_operator_type balance_operator )
    {
        FC_ASSERT(!delta.symbol.is_qi(), "Qi is not go there.");
        
        asset_symbol_type liquid_symbol = delta.symbol;
        const nfa_balance_object_type* bo = find< nfa_balance_object_type, by_nfa_liquid_symbol >( boost::make_tuple( nfa_id, liquid_symbol ) );

        if( bo == nullptr )
        {
            // No balance object related to the FA means '0' balance. Check delta to avoid creation of negative balance.
            FC_ASSERT( delta.amount.value >= 0, "Insufficient FA ${a} funds", ("a", delta.symbol) );
            // No need to create object with '0' balance (see comment above).
            if( delta.amount.value == 0 )
                return;

            create< nfa_balance_object_type >( [&]( nfa_balance_object_type& nfa_balance ) {
                nfa_balance.clear_balance( liquid_symbol );
                nfa_balance.nfa = nfa_id;
                balance_operator.add_to_balance( nfa_balance );
            } );
        }
        else
        {
            bool is_all_zero = false;
            int64_t result = balance_operator.get_combined_balance( bo, &is_all_zero );
            // Check result to avoid negative balance storing.
            FC_ASSERT( result >= 0, "Insufficient Assets ${as} funds", ( "as", delta.symbol ) );

            // Exit if whole balance becomes zero.
            if( is_all_zero )
            {
                // Zero balance is the same as non object balance at all.
                // Remove balance object if liquid balances is zero.
                remove( *bo );
            }
            else
            {
                modify( *bo, [&]( nfa_balance_object_type& nfa_balance ) {
                    balance_operator.add_to_balance( nfa_balance );
                } );
            }
        }
    }
    //=========================================================================
    struct nfa_regular_balance_operator
    {
        nfa_regular_balance_operator( const asset& delta ) : delta(delta) {}

        void add_to_balance( nfa_regular_balance_object& balance )
        {
            balance.liquid += delta;
        }
        int64_t get_combined_balance( const nfa_regular_balance_object* bo, bool* is_all_zero )
        {
            asset result = bo->liquid + delta;
            *is_all_zero = result.amount.value == 0;
            return result.amount.value;
        }

        asset delta;
    };

    void database::adjust_nfa_balance( const nfa_object& nfa, const asset& delta )
    {
        if ( delta.amount < 0 )
        {
            asset available = get_nfa_balance( nfa, delta.symbol );
            FC_ASSERT( available >= -delta,
                      "NFA ${id} does not have sufficient assets for balance adjustment. Required: ${r}, Available: ${a}",
                      ("id", nfa.id)("r", delta)("a", available) );
        }
        
        if( delta.symbol.asset_num == TAIYI_ASSET_NUM_QI) {
            modify(nfa, [&](nfa_object& obj) {
                obj.qi += delta;
            });
        }
        else {
            nfa_regular_balance_operator balance_operator( delta );
            adjust_nfa_balance< nfa_regular_balance_object >( nfa.id, delta, balance_operator );
        }
    }

} } //taiyi::chain
