#pragma once
#include <chain/taiyi_fwd.hpp>

#include <protocol/transaction.hpp>

#include <chain/buffer_type.hpp>
#include <chain/taiyi_object_types.hpp>

namespace taiyi { namespace chain {

    using taiyi::protocol::signed_transaction;

    /**
     * The purpose of this object is to enable the detection of duplicate transactions. When a transaction is included
     * in a block a transaction_object is added. At the end of block processing all transaction_objects that have
     * expired can be removed from the index.
     */
    class transaction_object : public object< transaction_object_type, transaction_object >
    {
        TAIYI_STD_ALLOCATOR_CONSTRUCTOR( transaction_object )
        
    public:
        template< typename Constructor, typename Allocator >
        transaction_object( Constructor&& c, allocator< Allocator > a )
            : packed_trx( a )
        {
            c( *this );
        }
        
        id_type              id;
        
        typedef buffer_type t_packed_trx;
        
        t_packed_trx         packed_trx;
        vector<protocol::operation_result> operation_results;
        transaction_id_type  trx_id;
        time_point_sec       expiration;
    };

    struct by_expiration;
    struct by_trx_id;
    typedef multi_index_container<
        transaction_object,
        indexed_by<
            ordered_unique< tag< by_id >, member< transaction_object, transaction_object_id_type, &transaction_object::id > >,
            ordered_unique< tag< by_trx_id >, member< transaction_object, transaction_id_type, &transaction_object::trx_id > >,
            ordered_unique< tag< by_expiration >,
                composite_key< transaction_object,
                    member<transaction_object, time_point_sec, &transaction_object::expiration >,
                    member<transaction_object, transaction_object::id_type, &transaction_object::id >
                >
            >
        >,
        allocator< transaction_object >
    > transaction_index;

} } // taiyi::chain

FC_REFLECT( taiyi::chain::transaction_object, (id)(packed_trx)(operation_results)(trx_id)(expiration) )
CHAINBASE_SET_INDEX_TYPE( taiyi::chain::transaction_object, taiyi::chain::transaction_index )

namespace helpers
{
    template <>
    class index_statistic_provider<taiyi::chain::transaction_index>
    {
    public:
        typedef taiyi::chain::transaction_index IndexType;
        typedef typename taiyi::chain::transaction_object::t_packed_trx t_packed_trx;
        
        index_statistic_info gather_statistics(const IndexType& index, bool onlyStaticInfo) const
        {
            index_statistic_info info;
            gather_index_static_data(index, &info);
            
            if(onlyStaticInfo == false)
            {
                for(const auto& o : index)
                {
                    info._item_additional_allocation += o.packed_trx.capacity()*sizeof(t_packed_trx::value_type);
                }
            }
            
            return info;
        }
    };

} /// namespace helpers
