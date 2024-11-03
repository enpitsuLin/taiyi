#include <chain/taiyi_fwd.hpp>

#include <chain/taiyi_evaluator.hpp>
#include <chain/database.hpp>
#include <chain/taiyi_objects.hpp>
#include <chain/account_object.hpp>
#include <chain/block_summary_object.hpp>
#include <chain/nfa_objects.hpp>
#include <chain/contract_objects.hpp>

#include <chain/util/manabar.hpp>

#include <fc/macros.hpp>

extern std::wstring utf8_to_wstring(const std::string& str);
extern std::string wstring_to_utf8(const std::wstring& str);

namespace taiyi { namespace chain {

operation_result create_nfa_symbol_evaluator::do_apply( const create_nfa_symbol_operation& o )
{ try {
    const auto& creator = _db.get_account( o.creator );
    FC_UNUSED(creator);
    
    const auto* nfa_symbol = _db.find<nfa_symbol_object, by_symbol>(o.symbol);
    FC_ASSERT(nfa_symbol == nullptr, "NFA symbol named \"${n}\" is already exist.", ("n", o.symbol));

    const auto& contract = _db.get<contract_object, by_name>(o.default_contract);
    auto abi_itr = contract.contract_ABI.find(lua_types(lua_string(TAIYI_NFA_INIT_FUNC_NAME)));
    FC_ASSERT(abi_itr != contract.contract_ABI.end(), "contract ${c} has not init function named ${i}", ("c", contract.name)("i", TAIYI_NFA_INIT_FUNC_NAME));
    
    _db.create<nfa_symbol_object>([&](nfa_symbol_object& obj) {
        obj.creator = creator.name;
        obj.symbol = o.symbol;
        obj.describe = o.describe;
        obj.default_contract = contract.id;
        obj.count = 0;
    });
    
    return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }
//=============================================================================
operation_result create_nfa_evaluator::do_apply( const create_nfa_operation& o )
{ try {
    const auto& creator = _db.get_account( o.creator );
    const auto* nfa_symbol = _db.find<nfa_symbol_object, by_symbol>(o.symbol);
    FC_ASSERT(nfa_symbol != nullptr, "NFA symbol named \"${n}\" is not exist.", ("n", o.symbol));
    
    const auto* current_trx = _db.get_current_trx_ptr();
    FC_ASSERT(current_trx);
    const flat_set<public_key_type>& sigkeys = current_trx->get_signature_keys(_db.get_chain_id(), fc::ecc::fc_canonical);

    const auto& nfa = _db.create_nfa(creator, *nfa_symbol, sigkeys);
    
    contract_result result;
    
    protocol::nfa_affected affected;
    affected.affected_account = creator.name;
    affected.affected_item = nfa.id;
    affected.action = nfa_affected_type::create_for;
    result.contract_affecteds.push_back(std::move(affected));
    
    affected.affected_account = creator.name;
    affected.action = nfa_affected_type::create_by;
    result.contract_affecteds.push_back(std::move(affected));

    return result;
} FC_CAPTURE_AND_RETHROW( (o) ) }
//=============================================================================
operation_result transfer_nfa_evaluator::do_apply( const transfer_nfa_operation& o )
{ try {
    const auto& from_account = _db.get_account(o.from);
    const auto& to_account = _db.get_account(o.to);

    const auto* nfa = _db.find<nfa_object, by_id>(o.id);
    FC_ASSERT(nfa != nullptr, "NFA with id ${i} not found.", ("i", o.id));

    FC_ASSERT(from_account.id == nfa->owner_account, "Can not transfer NFA not ownd by you.");
    
    _db.modify(*nfa, [&](nfa_object &obj) {
        obj.owner_account = to_account.id;
    });

    contract_result result;

    protocol::nfa_affected affected;
    affected.affected_account = o.from;
    affected.affected_item = nfa->id;
    affected.action = nfa_affected_type::transfer_from;
    result.contract_affecteds.push_back(std::move(affected));
    
    affected.affected_account = o.to;
    affected.affected_item = nfa->id;
    affected.action = nfa_affected_type::transfer_to;
    result.contract_affecteds.push_back(std::move(affected));

    return result;
} FC_CAPTURE_AND_RETHROW( (o) ) }

} } // taiyi::chain

