#include <chain/database_exceptions.hpp>

#include <plugins/chain/abstract_block_producer.hpp>
#include <plugins/chain/chain_plugin.hpp>

#include <utilities/benchmark_dumper.hpp>
#include <utilities/database_configuration.hpp>

#include <fc/string.hpp>
#include <fc/io/json.hpp>
#include <fc/io/fstream.hpp>

#include <boost/asio.hpp>
#include <boost/optional.hpp>
#include <boost/bind.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/thread/future.hpp>
#include <boost/lockfree/queue.hpp>

#include <thread>
#include <memory>
#include <iostream>

namespace taiyi { namespace plugins { namespace chain {

    using namespace taiyi;
    using fc::flat_map;
    using taiyi::chain::block_id_type;
    namespace asio = boost::asio;
    
#define NUM_THREADS 1
    
    struct generate_block_request
    {
        generate_block_request( const fc::time_point_sec w, const account_name_type& wo, const fc::ecc::private_key& priv_key, uint32_t s ) :
        when( w ), siming_owner( wo ), block_signing_private_key( priv_key ), skip( s ) {}
        
        const fc::time_point_sec when;
        const account_name_type& siming_owner;
        const fc::ecc::private_key& block_signing_private_key;
        uint32_t skip;
        signed_block block;
    };

    typedef fc::static_variant<
        const signed_block*,
        const signed_transaction*,
        generate_block_request*
    > write_request_ptr;

    typedef fc::static_variant<
        boost::promise< void >*,
        fc::future< void >*
    > promise_ptr;

    struct write_context
    {
        write_request_ptr             req_ptr;
        uint32_t                      skip = 0;
        bool                          success = true;
        fc::optional< fc::exception > except;
        promise_ptr                   prom_ptr;
    };

    namespace detail {
        
        class chain_plugin_impl
        {
        public:
            chain_plugin_impl() : write_queue( 64 ) {}
            ~chain_plugin_impl() { stop_write_processing(); }
            
            void start_write_processing();
            void stop_write_processing();
            void write_default_database_config( bfs::path& p );
            
            uint32_t                         chainbase_flags = 0;
            bfs::path                        state_storage_dir;
            bool                             replay = false;
            bool                             resync   = false;
            bool                             readonly = false;
            bool                             check_locks = false;
            bool                             validate_invariants = false;
            bool                             dump_memory_details = false;
            bool                             benchmark_is_enabled = false;
            uint32_t                         stop_replay_at = 0;
            uint32_t                         benchmark_interval = 0;
            uint32_t                         flush_interval = 0;
            bool                             replay_in_memory = false;
            std::vector< std::string >       replay_memory_indices{};
            flat_map<uint32_t,block_id_type> loaded_checkpoints;
            
            uint32_t                         allow_future_time = 5;
            
            bool                             running = true;
            std::shared_ptr< std::thread >   write_processor_thread;
            boost::lockfree::queue< write_context* > write_queue;
            int16_t                          write_lock_hold_time = 500;
            
            vector< string >                 loaded_plugins;
            fc::mutable_variant_object       plugin_state_opts;
            bfs::path                        database_cfg;
            
            database                         db;
            std::string                      block_generator_registrant;
            std::shared_ptr< abstract_block_producer > block_generator;
        };

        struct write_request_visitor
        {
            write_request_visitor() {}
            
            database* db;
            uint32_t  skip = 0;
            fc::optional< fc::exception >* except;
            std::shared_ptr< abstract_block_producer > block_generator;
            
            typedef bool result_type;
            
            bool operator()( const signed_block* block )
            {
                bool result = false;
                
                try
                {
                    result = db->push_block( *block, skip );
                }
                catch( fc::exception& e )
                {
                    *except = e;
                }
                catch( ... )
                {
                    *except = fc::unhandled_exception( FC_LOG_MESSAGE( warn, "Unexpected exception while pushing block." ), std::current_exception() );
                }
                
                return result;
            }

            bool operator()( const signed_transaction* trx )
            {
                bool result = false;
                
                try
                {
                    db->push_transaction( *trx );
                    
                    result = true;
                }
                catch( fc::exception& e )
                {
                    *except = e;
                }
                catch( ... )
                {
                    *except = fc::unhandled_exception( FC_LOG_MESSAGE( warn, "Unexpected exception while pushing block." ), std::current_exception() );
                }
                
                return result;
            }

            bool operator()( generate_block_request* req )
            {
                bool result = false;
                
                try
                {
                    if( !block_generator )
                        FC_THROW_EXCEPTION( chain_exception, "Received a generate block request, but no block generator has been registered." );
                    
                    req->block = block_generator->generate_block(req->when, req->siming_owner, req->block_signing_private_key, req->skip);
                    
                    result = true;
                }
                catch( fc::exception& e )
                {
                    *except = e;
                }
                catch( ... )
                {
                    *except = fc::unhandled_exception( FC_LOG_MESSAGE( warn, "Unexpected exception while pushing block." ), std::current_exception() );
                }
                
                return result;
            }
        };

        struct request_promise_visitor
        {
            request_promise_visitor(){}
            
            typedef void result_type;
            
            template< typename T >
            void operator()( T* t )
            {
                t->set_value();
            }
        };

        void chain_plugin_impl::start_write_processing()
        {
            write_processor_thread = std::make_shared< std::thread >( [&]() {
                bool is_syncing = true;
                write_context* cxt;
                fc::time_point_sec start = fc::time_point::now();
                write_request_visitor req_visitor;
                req_visitor.db = &db;
                req_visitor.block_generator = block_generator;
                
                request_promise_visitor prom_visitor;
                
                /* This loop monitors the write request queue and performs writes to the database. These
                 * can be blocks or pending transactions. Because the caller needs to know the success of
                 * the write and any exceptions that are thrown, a write context is passed in the queue
                 * to the processing thread which it will use to store the results of the write. It is the
                 * caller's responsibility to ensure the pointer to the write context remains valid until
                 * the contained promise is complete.
                 *
                 * The loop has two modes, sync mode and live mode. In sync mode we want to process writes
                 * as quickly as possible with minimal overhead. The outer loop busy waits on the queue
                 * and the inner loop drains the queue as quickly as possible. We exit sync mode when the
                 * head block is within 1 minute of system time.
                 *
                 * Live mode needs to balance between processing pending writes and allowing readers access
                 * to the database. It will batch writes together as much as possible to minimize lock
                 * overhead but will willingly give up the write lock after 500ms. The thread then sleeps for
                 * 10ms. This allows time for readers to access the database as well as more writes to come
                 * in. When the node is live the rate at which writes come in is slower and busy waiting is
                 * not an optimal use of system resources when we could give CPU time to read threads.
                 */
                while( running )
                {
                    if( !is_syncing )
                        start = fc::time_point::now();
                    
                    if( write_queue.pop( cxt ) )
                    {
                        db.with_write_lock( [&]() {
                            while( true )
                            {
                                req_visitor.skip = cxt->skip;
                                req_visitor.except = &(cxt->except);
                                cxt->success = cxt->req_ptr.visit( req_visitor );
                                cxt->prom_ptr.visit( prom_visitor );
                                
                                if( is_syncing && start - db.head_block_time() < fc::minutes(1) )
                                {
                                    start = fc::time_point::now();
                                    is_syncing = false;
                                }
                                
                                if( !is_syncing && write_lock_hold_time >= 0 && fc::time_point::now() - start > fc::milliseconds( write_lock_hold_time ) )
                                {
                                    break;
                                }
                                
                                if( !write_queue.pop( cxt ) )
                                {
                                    break;
                                }
                            }
                        });
                    }
                    
                    if( !is_syncing )
                        boost::this_thread::sleep_for( boost::chrono::milliseconds( 10 ) );
                }
            });
        }

        void chain_plugin_impl::stop_write_processing()
        {
            running = false;
            
            if( write_processor_thread )
                write_processor_thread->join();
            
            write_processor_thread.reset();
        }

        void chain_plugin_impl::write_default_database_config( bfs::path &p )
        {
            ilog( "writing database configuration: ${p}", ("p", p.string()) );
            fc::json::save_to_file( taiyi::utilities::default_database_configuration(), p );
        }
        
    } // detail
    

    chain_plugin::chain_plugin() : my( new detail::chain_plugin_impl() ) {}
    chain_plugin::~chain_plugin(){}
    
    database& chain_plugin::db() { return my->db; }
    const taiyi::chain::database& chain_plugin::db() const { return my->db; }

    bfs::path chain_plugin::state_storage_dir() const
    {
        return my->state_storage_dir;
    }
    
    void chain_plugin::set_program_options(options_description& cli, options_description& cfg)
    {
        cfg.add_options()
            ("state-storage-dir", bpo::value<bfs::path>()->default_value("blockchain"), "the location of the chain state memory or database files (absolute path or relative to application data dir)")
            ("checkpoint,c", bpo::value<vector<string>>()->composing(), "Pairs of [BLOCK_NUM,BLOCK_ID] that should be enforced as checkpoints.")
            ("flush-state-interval", bpo::value<uint32_t>(), "flush state changes to disk every N blocks")
            ("memory-replay-indices", bpo::value<vector<string>>()->multitoken()->composing(), "Specify which indices should be in memory during replay")
            ;
        cli.add_options()
            ("replay-blockchain", bpo::bool_switch()->default_value(false), "clear chain database and replay all blocks" )
            ("force-open", bpo::bool_switch()->default_value(false), "force open the database, skipping the environment check" )
            ("resync-blockchain", bpo::bool_switch()->default_value(false), "clear chain database and block log" )
            ("stop-replay-at-block", bpo::value<uint32_t>(), "Stop and exit after reaching given block number")
            ("advanced-benchmark", "Make profiling for every plugin.")
            ("set-benchmark-interval", bpo::value<uint32_t>(), "Print time and memory usage every given number of blocks")
            ("dump-memory-details", bpo::bool_switch()->default_value(false), "Dump database objects memory usage info. Use set-benchmark-interval to set dump interval.")
            ("check-locks", bpo::bool_switch()->default_value(false), "Check correctness of chainbase locking" )
            ("validate-database-invariants", bpo::bool_switch()->default_value(false), "Validate all supply invariants check out" )
            ("database-cfg", bpo::value<bfs::path>()->default_value("database.cfg"), "The database configuration file location")
            ("memory-replay,m", bpo::bool_switch()->default_value(false), "Replay with state in memory instead of on disk")
#ifdef IS_TEST_NET
            ("chain-id", bpo::value< std::string >()->default_value( TAIYI_CHAIN_ID ), "chain ID to connect to")
#endif
            ;
    }

    void chain_plugin::plugin_initialize(const variables_map& options)
    {
        my->state_storage_dir = app().data_dir() / "blockchain";
        if( options.count("state-storage-dir") )
        {
            auto ssd = options.at("state-storage-dir").as<bfs::path>();
            if(ssd.is_relative())
                my->state_storage_dir = app().data_dir() / ssd;
            else
                my->state_storage_dir = ssd;
        }
        
        my->chainbase_flags |= options.at( "force-open" ).as< bool >() ? chainbase::skip_env_check : chainbase::skip_nothing;
        
        my->replay              = options.at( "replay-blockchain").as<bool>();
        my->resync              = options.at( "resync-blockchain").as<bool>();
        my->stop_replay_at      = options.count( "stop-replay-at-block" ) ? options.at( "stop-replay-at-block" ).as<uint32_t>() : 0;
        my->benchmark_interval  = options.count( "set-benchmark-interval" ) ? options.at( "set-benchmark-interval" ).as<uint32_t>() : 0;
        my->check_locks         = options.at( "check-locks" ).as< bool >();
        my->validate_invariants = options.at( "validate-database-invariants" ).as<bool>();
        my->dump_memory_details = options.at( "dump-memory-details" ).as<bool>();
        if( options.count( "flush-state-interval" ) )
            my->flush_interval = options.at( "flush-state-interval" ).as<uint32_t>();
        else
            my->flush_interval = 10000;

        if(options.count("checkpoint"))
        {
            auto cps = options.at("checkpoint").as<vector<string>>();
            my->loaded_checkpoints.reserve(cps.size());
            for(const auto& cp : cps)
            {
                auto item = fc::json::from_string(cp).as<std::pair<uint32_t,block_id_type>>();
                my->loaded_checkpoints[item.first] = item.second;
            }
        }

        my->benchmark_is_enabled = (options.count( "advanced-benchmark" ) != 0);
                
        my->database_cfg = options.at( "database-cfg" ).as< bfs::path >();
        
        if( my->database_cfg.is_relative() )
            my->database_cfg = app().data_dir() / my->database_cfg;
        
        if( !bfs::exists( my->database_cfg ) )
        {
            my->write_default_database_config( my->database_cfg );
        }
        
        my->replay_in_memory = options.at( "memory-replay" ).as< bool >();
        if ( options.count( "memory-replay-indices" ) )
        {
            std::vector<std::string> indices = options.at( "memory-replay-indices" ).as< vector< string > >();
            for ( auto& element : indices )
            {
                std::vector< std::string > tmp;
                boost::split( tmp, element, boost::is_any_of("\t ") );
                my->replay_memory_indices.insert( my->replay_memory_indices.end(), tmp.begin(), tmp.end() );
            }
        }

#ifdef IS_TEST_NET
        if( options.count( "chain-id" ) )
        {
            auto chain_id_str = options.at("chain-id").as< std::string >();
            
            try
            {
                my->db.set_chain_id( chain_id_type( chain_id_str) );
            }
            catch( fc::exception& )
            {
                FC_ASSERT( false, "Could not parse chain_id as hex string. Chain ID String: ${s}", ("s", chain_id_str) );
            }
        }
#endif
    }

#define BENCHMARK_FILE_NAME "replay_benchmark.json"

    void chain_plugin::plugin_startup()
    {        
        if(my->resync)
        {
            wlog("resync requested: deleting block log and state memory");
            my->db.wipe( app().data_dir() / "blockchain", my->state_storage_dir, true );
        }
        
        my->db.set_flush_interval( my->flush_interval );
        my->db.add_checkpoints( my->loaded_checkpoints );
        my->db.set_require_locking( my->check_locks );
        
        bool dump_memory_details = my->dump_memory_details;
        taiyi::utilities::benchmark_dumper dumper;
        
        const auto& abstract_index_cntr = my->db.get_abstract_index_cntr();
        
        typedef taiyi::utilities::benchmark_dumper::index_memory_details_cntr_t index_memory_details_cntr_t;
        auto get_indexes_memory_details = [dump_memory_details, &abstract_index_cntr] (index_memory_details_cntr_t& index_memory_details_cntr, bool onlyStaticInfo) {
            if (dump_memory_details == false)
                return;
            
            for (auto idx : abstract_index_cntr)
            {
                auto info = idx->get_statistics(onlyStaticInfo);
                index_memory_details_cntr.emplace_back(std::move(info._value_type_name), info._item_count, info._item_sizeof, info._item_additional_allocation, info._additional_container_allocation);
            }
        };
        
        fc::variant database_config;
        
        try
        {
            database_config = fc::json::from_file( my->database_cfg, fc::json::strict_parser );
        }
        catch ( const std::exception& e )
        {
            elog( "Error while parsing database configuration: ${e}", ("e", e.what()) );
            exit( EXIT_FAILURE );
        }
        catch ( const fc::exception& e )
        {
            elog( "Error while parsing database configuration: ${e}", ("e", e.what()) );
            exit( EXIT_FAILURE );
        }
        
        database::open_args db_open_args;
        db_open_args.data_dir = app().data_dir() / "blockchain";
        db_open_args.state_storage_dir = my->state_storage_dir;
        db_open_args.initial_supply = TAIYI_YANG_INIT_SUPPLY;
        db_open_args.chainbase_flags = my->chainbase_flags;
        db_open_args.do_validate_invariants = my->validate_invariants;
        db_open_args.stop_replay_at = my->stop_replay_at;
        db_open_args.benchmark_is_enabled = my->benchmark_is_enabled;
        db_open_args.database_cfg = database_config;
        db_open_args.replay_in_memory = my->replay_in_memory;
        db_open_args.replay_memory_indices = my->replay_memory_indices;

        auto benchmark_lambda = [&dumper, &get_indexes_memory_details, dump_memory_details] ( uint32_t current_block_number, const chainbase::database::abstract_index_cntr_t& abstract_index_cntr ) {
            if( current_block_number == 0 ) // initial call
            {
                typedef taiyi::utilities::benchmark_dumper::database_object_sizeof_cntr_t database_object_sizeof_cntr_t;
                auto get_database_objects_sizeofs = [dump_memory_details, &abstract_index_cntr] (database_object_sizeof_cntr_t& database_object_sizeof_cntr) {
                    if (dump_memory_details == false)
                        return;
                    
                    for (auto idx : abstract_index_cntr)
                    {
                        auto info = idx->get_statistics(true);
                        database_object_sizeof_cntr.emplace_back(std::move(info._value_type_name), info._item_sizeof);
                    }
                };
                
                dumper.initialize(get_database_objects_sizeofs, BENCHMARK_FILE_NAME);
                return;
            }
            
            const taiyi::utilities::benchmark_dumper::measurement& measure = dumper.measure(current_block_number, get_indexes_memory_details);
            ilog( "Performance report at block ${n}. Elapsed time: ${rt} ms (real), ${ct} ms (cpu). Memory usage: ${cm} (current), ${pm} (peak) kilobytes.", ("n", current_block_number)("rt", measure.real_ms)("ct", measure.cpu_ms)("cm", measure.current_mem)("pm", measure.peak_mem) );
        };

        if(my->replay)
        {
            ilog("Replaying blockchain on user request.");
            uint32_t last_block_number = 0;
            db_open_args.benchmark = taiyi::chain::database::TBenchmark(my->benchmark_interval, benchmark_lambda);
            last_block_number = my->db.reindex( db_open_args );
            
            if( my->benchmark_interval > 0 )
            {
                const taiyi::utilities::benchmark_dumper::measurement& total_data = dumper.dump(true, get_indexes_memory_details);
                ilog( "Performance report (total). Blocks: ${b}. Elapsed time: ${rt} ms (real), ${ct} ms (cpu). Memory usage: ${cm} (current), ${pm} (peak) kilobytes.", ("b", total_data.block_number)("rt", total_data.real_ms)("ct", total_data.cpu_ms)("cm", total_data.current_mem)("pm", total_data.peak_mem) );
            }
            
            if( my->stop_replay_at > 0 && my->stop_replay_at == last_block_number )
            {
                ilog("Stopped blockchain replaying on user request. Last applied block number: ${n}.", ("n", last_block_number));
                exit(EXIT_SUCCESS);
            }
        }
        else
        {
            db_open_args.benchmark = taiyi::chain::database::TBenchmark(dump_memory_details, benchmark_lambda);
            
            try
            {
                ilog("Opening state data from ${path}", ("path",my->state_storage_dir.generic_string()));
                
                my->db.open( db_open_args );
                
                if( dump_memory_details )
                    dumper.dump( true, get_indexes_memory_details );
            }
            catch( const fc::exception& e )
            {
                wlog( "Error opening database. If the binary or configuration has changed, replay the blockchain explicitly using `--replay-blockchain`." );
                wlog( "If you know what you are doing you can skip this check and force open the database using `--force-open`." );
                wlog( "WARNING: THIS MAY CORRUPT YOUR DATABASE. FORCE OPEN AT YOUR OWN RISK." );
                wlog( " Error: ${e}", ("e", e) );
                exit(EXIT_FAILURE);
            }
        }
        
        ilog( "Started on blockchain with ${n} blocks", ("n", my->db.head_block_num()) );
        on_sync();
        
        my->start_write_processing();
    }

    void chain_plugin::plugin_shutdown()
    {
        ilog("closing chain database");
        my->stop_write_processing();
        my->db.close();
        ilog("database closed successfully");
    }
    
    void chain_plugin::report_state_options( const string& plugin_name, const fc::variant_object& opts )
    {
        my->loaded_plugins.push_back( plugin_name );
        my->plugin_state_opts( opts );
    }

    bool chain_plugin::accept_block( const taiyi::chain::signed_block& block, bool currently_syncing, uint32_t skip )
    {
        if (currently_syncing && block.block_num() % 10000 == 0) {
            ilog("Syncing Blockchain --- Got block: #${n} time: ${t} producer: ${p}", ("t", block.timestamp)("n", block.block_num())("p", block.siming) );
        }
        
        check_time_in_block( block );
        
        boost::promise< void > prom;
        write_context cxt;
        cxt.req_ptr = &block;
        cxt.skip = skip;
        cxt.prom_ptr = &prom;
        
        my->write_queue.push( &cxt );
        
        prom.get_future().get();
        
        if( cxt.except ) throw *(cxt.except);
        
        return cxt.success;
    }

    void chain_plugin::accept_transaction( const taiyi::chain::signed_transaction& trx )
    {
        boost::promise< void > prom;
        write_context cxt;
        cxt.req_ptr = &trx;
        cxt.prom_ptr = &prom;
        
        my->write_queue.push( &cxt );
        
        prom.get_future().get();
        
        if( cxt.except ) throw *(cxt.except);
        
        return;
    }
    
    taiyi::chain::signed_block chain_plugin::generate_block(const fc::time_point_sec when, const account_name_type& siming_owner, const fc::ecc::private_key& block_signing_private_key, uint32_t skip )
    {
        generate_block_request req( when, siming_owner, block_signing_private_key, skip );
        boost::promise< void > prom;
        write_context cxt;
        cxt.req_ptr = &req;
        cxt.prom_ptr = &prom;
        
        my->write_queue.push( &cxt );
        
        prom.get_future().get();
        
        if( cxt.except ) throw *(cxt.except);
        
        FC_ASSERT( cxt.success, "Block could not be generated" );
        
        return req.block;
    }
    
    int16_t chain_plugin::set_write_lock_hold_time( int16_t new_time )
    {
        FC_ASSERT( get_state() == appbase::abstract_plugin::state::initialized, "Can only change write_lock_hold_time while chain_plugin is initialized." );
        
        int16_t old_time = my->write_lock_hold_time;
        my->write_lock_hold_time = new_time;
        return old_time;
    }
    
    bool chain_plugin::block_is_on_preferred_chain(const taiyi::chain::block_id_type& block_id )
    {
        // If it's not known, it's not preferred.
        if( !db().is_known_block(block_id) ) return false;
        
        // Extract the block number from block_id, and fetch that block number's ID from the database.
        // If the database's block ID matches block_id, then block_id is on the preferred chain. Otherwise, it's on a fork.
        return db().get_block_id_for_num( taiyi::chain::block_header::num_from_id( block_id ) ) == block_id;
    }
    
    void chain_plugin::check_time_in_block( const taiyi::chain::signed_block& block )
    {
        time_point_sec now = fc::time_point::now();
        
        uint64_t max_accept_time = now.sec_since_epoch();
        max_accept_time += my->allow_future_time;
        FC_ASSERT( block.timestamp.sec_since_epoch() <= max_accept_time );
    }
    
    void chain_plugin::register_block_generator( const std::string& plugin_name, std::shared_ptr< abstract_block_producer > block_producer )
    {
        FC_ASSERT( get_state() == appbase::abstract_plugin::state::initialized, "Can only register a block generator when the chain_plugin is initialized." );
        
        if ( my->block_generator )
            wlog( "Overriding a previously registered block generator by: ${registrant}", ("registrant", my->block_generator_registrant) );
        
        my->block_generator_registrant = plugin_name;
        my->block_generator = block_producer;
    }
    
} } } // namespace taiyi::plugis::chain::chain_apis
