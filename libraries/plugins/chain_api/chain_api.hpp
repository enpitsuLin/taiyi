#pragma once
#include <plugins/json_rpc/utility.hpp>

#include <protocol/types.hpp>

#include <fc/optional.hpp>

namespace taiyi { namespace plugins { namespace chain {

    namespace detail { class chain_api_impl; }
    
    struct push_block_args
    {
        taiyi::chain::signed_block block;
        bool                         currently_syncing = false;
    };

    struct push_block_return
    {
        bool              success;
        optional<string>  error;
    };
    
    typedef taiyi::chain::signed_transaction push_transaction_args;
    
    struct push_transaction_return
    {
        bool              success;
        optional<string>  error;
    };

    class chain_api
    {
    public:
        chain_api();
        ~chain_api();
        
        DECLARE_API(
            (push_block)
            (push_transaction)
        )
        
    private:
        std::unique_ptr< detail::chain_api_impl > my;
    };

} } } // taiyi::plugins::chain

FC_REFLECT( taiyi::plugins::chain::push_block_args, (block)(currently_syncing) )
FC_REFLECT( taiyi::plugins::chain::push_block_return, (success)(error) )
FC_REFLECT( taiyi::plugins::chain::push_transaction_return, (success)(error) )
