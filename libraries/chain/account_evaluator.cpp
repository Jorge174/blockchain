/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <fc/smart_ref_impl.hpp>

#include <graphene/chain/account_evaluator.hpp>
#include <graphene/chain/buyback.hpp>
#include <graphene/chain/buyback_object.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/internal_exceptions.hpp>
#include <graphene/chain/special_authority.hpp>
#include <graphene/chain/special_authority_object.hpp>
#include <graphene/chain/worker_object.hpp>
#include <iostream>
#include <algorithm>

namespace graphene { namespace chain {

void verify_authority_accounts( const database& db, const authority& a )
{
   const auto& chain_params = db.get_global_properties().parameters;
   GRAPHENE_ASSERT(
      a.num_auths() <= chain_params.maximum_authority_membership,
      internal_verify_auth_max_auth_exceeded,
      "Maximum authority membership exceeded" );
   for( const auto& acnt : a.account_auths )
   {
      GRAPHENE_ASSERT( db.find_object( acnt.first ) != nullptr,
         internal_verify_auth_account_not_found,
         "Account ${a} specified in authority does not exist",
         ("a", acnt.first) );
   }
}

void check_accounts_usage(database& _db, set<account_id_type> new_accs, set<public_key_type> new_keys)
{
   for( auto key : new_keys)
   {
      int count = 0;
      const auto& idx = _db.get_index_type<account_index>();
      const auto& aidx = dynamic_cast<const primary_index<account_index>&>(idx);
      const auto& refs = aidx.get_secondary_index<graphene::chain::account_member_index>();
      auto itr = refs.account_to_key_memberships.find(key);

      // wtf: no more than 3 equal keys for one account?
      if( itr != refs.account_to_key_memberships.end() )
      {
         for( auto item : itr->second )
         {
            (void)item;

            count++;
            FC_ASSERT(count < 3); // can be 0 - 12 without assert
         }
      }
   }

   const auto& idx = _db.get_index_type<account_index>();
   const auto& aidx = dynamic_cast<const primary_index<account_index>&>(idx);
   const auto& refs = aidx.get_secondary_index<graphene::chain::account_member_index>();

   for( auto acc : new_accs)
   {
      int count = 0;
      auto itr = refs.account_to_account_memberships.find(acc);

      if( itr != refs.account_to_account_memberships.end() )
      {
         for( auto item : itr->second )
         {
            (void)item;
            count++;
            FC_ASSERT(count < 3);
         }
      }
   }
}

void verify_account_votes( const database& db, const account_options& options )
{
   // ensure account's votes satisfy requirements
   // NB only the part of vote checking that requires chain state is here,
   // the rest occurs in account_options::validate()

   const auto& gpo = db.get_global_properties();
   const auto& chain_params = gpo.parameters;

   FC_ASSERT( options.num_witness <= chain_params.maximum_witness_count,
              "Voted for more witnesses than currently allowed (${c})", ("c", chain_params.maximum_witness_count) );
   FC_ASSERT( options.num_committee <= chain_params.maximum_committee_count,
              "Voted for more committee members than currently allowed (${c})", ("c", chain_params.maximum_committee_count) );

   uint32_t max_vote_id = gpo.next_available_vote_id;
   bool has_worker_votes = false;
   for( auto id : options.votes )
   {
      FC_ASSERT( id < max_vote_id );
      has_worker_votes |= (id.type() == vote_id_type::worker);
   }

   if( has_worker_votes && (db.head_block_time() >= HARDFORK_607_TIME) )
   {
      const auto& against_worker_idx = db.get_index_type<worker_index>().indices().get<by_vote_against>();
      for( auto id : options.votes )
      {
         if( id.type() == vote_id_type::worker )
         {
            FC_ASSERT( against_worker_idx.find( id ) == against_worker_idx.end() );
         }
      }
   }

}

void_result account_create_evaluator::do_evaluate( const account_create_operation& op )
{ try {
   database& d = db();
   if( d.head_block_time() < HARDFORK_516_TIME )
   {
      FC_ASSERT( !op.extensions.value.owner_special_authority.valid() );
      FC_ASSERT( !op.extensions.value.active_special_authority.valid() );
   }
   if( d.head_block_time() < HARDFORK_599_TIME )
   {
      FC_ASSERT( !op.extensions.value.null_ext.valid() );
      FC_ASSERT( !op.extensions.value.owner_special_authority.valid() );
      FC_ASSERT( !op.extensions.value.active_special_authority.valid() );
      FC_ASSERT( !op.extensions.value.buyback_options.valid() );
   }

   if (d.head_block_time() > HARDFORK_617_TIME)
   {
      set<account_id_type> accs;
      set<public_key_type> keys;
      auto va = op.owner.get_accounts();
      auto vk = op.owner.get_keys();
      accs.insert(va.begin(), va.end());
      keys.insert(vk.begin(), vk.end());
      va = op.active.get_accounts();
      vk = op.active.get_keys();
      accs.insert(va.begin(), va.end());
      keys.insert(vk.begin(), vk.end());

      if (!db().referrer_mode_is_enabled()) {
         check_accounts_usage(d, accs, keys);
      }

      auto& p = d.get_account_properties();
      auto prop_itr = p.accounts_properties.find(op.registrar);

      if (!db().referrer_mode_is_enabled())
      {
         FC_ASSERT(prop_itr != p.accounts_properties.end());
         FC_ASSERT(prop_itr->second.can_be_referrer);
      }
   }

   FC_ASSERT( d.find_object(op.options.voting_account), "Invalid proxy account specified." );
//   FC_ASSERT( fee_paying_account->is_lifetime_member(), "Only Lifetime members may register an account." );
//   FC_ASSERT( op.referrer(d).is_member(d.head_block_time()), "The referrer must be either a lifetime or annual subscriber." );

   try
   {
      verify_authority_accounts( d, op.owner );
      verify_authority_accounts( d, op.active );
   }
   GRAPHENE_RECODE_EXC( internal_verify_auth_max_auth_exceeded, account_create_max_auth_exceeded )
   GRAPHENE_RECODE_EXC( internal_verify_auth_account_not_found, account_create_auth_account_not_found )

   if( op.extensions.value.owner_special_authority.valid() )
      evaluate_special_authority( d, *op.extensions.value.owner_special_authority );
   if( op.extensions.value.active_special_authority.valid() )
      evaluate_special_authority( d, *op.extensions.value.active_special_authority );
   if( op.extensions.value.buyback_options.valid() )
      evaluate_buyback_account_options( d, *op.extensions.value.buyback_options );
   verify_account_votes( d, op.options );

   auto& acnt_indx = d.get_index_type<account_index>();
   if( op.name.size() )
   {
      auto current_account_itr = acnt_indx.indices().get<by_name>().find( op.name );

      FC_ASSERT( current_account_itr == acnt_indx.indices().get<by_name>().end() );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type account_create_evaluator::do_apply( const account_create_operation& o )
{ try {

   database& d = db();
   uint16_t referrer_percent = o.referrer_percent;
   bool has_small_percent = (
         (db().head_block_time() <= HARDFORK_453_TIME)
      && (o.referrer != o.registrar  )
      && (o.referrer_percent != 0    )
      && (o.referrer_percent <= 0x100)
      );
   if( has_small_percent )
   {
      if( referrer_percent >= 100 )
      {
         wlog( "between 100% and 0x100%:  ${o}", ("o", o) );
      }
      referrer_percent = referrer_percent*100;
      if( referrer_percent > GRAPHENE_100_PERCENT )
         referrer_percent = GRAPHENE_100_PERCENT;
   }

   const auto& new_acnt_object = db().create<account_object>( [&]( account_object& obj ){
//         if( o.extensions.value.is_market.valid() )
//            obj.is_market = *o.extensions.value.is_market;
         obj.registrar = o.registrar;
         obj.referrer = o.referrer;
         obj.lifetime_referrer = o.referrer(db()).lifetime_referrer;

         auto& params = db().get_global_properties().parameters;
         obj.network_fee_percentage = params.network_percent_of_fee;
         obj.lifetime_referrer_fee_percentage = params.lifetime_referrer_percent_of_fee;
         obj.referrer_rewards_percentage = referrer_percent;

         obj.name             = o.name;
         obj.owner            = o.owner;
         obj.active           = o.active;
         obj.options          = o.options;
         obj.statistics = db().create<account_statistics_object>([&](account_statistics_object& s){s.owner = obj.id;}).id;

         if( o.extensions.value.owner_special_authority.valid() )
            obj.owner_special_authority = *(o.extensions.value.owner_special_authority);
         if( o.extensions.value.active_special_authority.valid() )
            obj.active_special_authority = *(o.extensions.value.active_special_authority);
         if( o.extensions.value.buyback_options.valid() )
         {
            obj.allowed_assets = o.extensions.value.buyback_options->markets;
            obj.allowed_assets->emplace( o.extensions.value.buyback_options->asset_to_buy );
         }
   });

   if( has_small_percent )
   {
      wlog( "Account affected by #453 registered in block ${n}:  ${na} reg=${reg} ref=${ref}:${refp} ltr=${ltr}:${ltrp}",
         ("n", db().head_block_num()) ("na", new_acnt_object.id)
         ("reg", o.registrar) ("ref", o.referrer) ("ltr", new_acnt_object.lifetime_referrer)
         ("refp", new_acnt_object.referrer_rewards_percentage) ("ltrp", new_acnt_object.lifetime_referrer_fee_percentage) );
      wlog( "Affected account object is ${o}", ("o", new_acnt_object) );
   }

   const auto& dynamic_properties = db().get_dynamic_global_properties();
   db().modify(dynamic_properties, [](dynamic_global_property_object& p) {
      ++p.accounts_registered_this_interval;
   });

   const auto& global_properties = db().get_global_properties();
   if( dynamic_properties.accounts_registered_this_interval %
       global_properties.parameters.accounts_per_fee_scale == 0 )
      db().modify(global_properties, [&dynamic_properties](global_property_object& p) {
         p.parameters.current_fees->get<account_create_operation>().basic_fee <<= p.parameters.account_fee_scale_bitshifts;
      });

   if(    o.extensions.value.owner_special_authority.valid()
       || o.extensions.value.active_special_authority.valid() )
   {
      db().create< special_authority_object >( [&]( special_authority_object& sa )
      {
         sa.account = new_acnt_object.id;
      } );
   }

   if( o.extensions.value.buyback_options.valid() )
   {
      asset_id_type asset_to_buy = o.extensions.value.buyback_options->asset_to_buy;

      d.create< buyback_object >( [&]( buyback_object& bo )
      {
         bo.asset_to_buy = asset_to_buy;
      } );

      d.modify( asset_to_buy(d), [&]( asset_object& a )
      {
         a.buyback_account = new_acnt_object.id;
      } );
   }

   return new_acnt_object.id;
} FC_CAPTURE_AND_RETHROW((o)) }


void_result account_update_evaluator::do_evaluate( const account_update_operation& o )
{ try {
   database& d = db();
   if( d.head_block_time() < HARDFORK_516_TIME )
   {
      FC_ASSERT( !o.extensions.value.owner_special_authority.valid() );
      FC_ASSERT( !o.extensions.value.active_special_authority.valid() );
   }
   if( d.head_block_time() < HARDFORK_599_TIME )
   {
      FC_ASSERT( !o.extensions.value.null_ext.valid() );
      FC_ASSERT( !o.extensions.value.owner_special_authority.valid() );
      FC_ASSERT( !o.extensions.value.active_special_authority.valid() );
   }

   if (d.head_block_time() > HARDFORK_617_TIME)
   {
      set<account_id_type> accs;
      set<public_key_type> keys;
      if (o.owner)
      {
         auto va = (*o.owner).get_accounts();
         auto vk = (*o.owner).get_keys();
         accs.insert(va.begin(), va.end());
         keys.insert(vk.begin(), vk.end());
      }
      if (o.active)
      {
         auto va = (*o.active).get_accounts();
         auto vk = (*o.active).get_keys();
         accs.insert(va.begin(), va.end());
         keys.insert(vk.begin(), vk.end());
      }

      if (!db().referrer_mode_is_enabled()) {
         check_accounts_usage(d, accs, keys);
      }
   }

   try
   {
      if( o.owner )  verify_authority_accounts( d, *o.owner );
      if( o.active ) verify_authority_accounts( d, *o.active );
   }
   GRAPHENE_RECODE_EXC( internal_verify_auth_max_auth_exceeded, account_update_max_auth_exceeded )
   GRAPHENE_RECODE_EXC( internal_verify_auth_account_not_found, account_update_auth_account_not_found )

   if( o.extensions.value.owner_special_authority.valid() )
      evaluate_special_authority( d, *o.extensions.value.owner_special_authority );
   if( o.extensions.value.active_special_authority.valid() )
      evaluate_special_authority( d, *o.extensions.value.active_special_authority );

   acnt = &o.account(d);

   if( o.new_options.valid() )
      verify_account_votes( d, *o.new_options );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result account_update_evaluator::do_apply( const account_update_operation& o )
{ try {
   database& d = db();
   bool sa_before, sa_after;

   d.modify( *acnt, [&](account_object& a){
      if( o.owner )
      {
         a.owner = *o.owner;
         a.top_n_control_flags = 0;
      }
      if( o.active )
      {
         a.active = *o.active;
         a.top_n_control_flags = 0;
      }
      if( o.new_options ) a.options = *o.new_options;
      sa_before = a.has_special_authority();
      if( o.extensions.value.owner_special_authority.valid() )
      {
         a.owner_special_authority = *(o.extensions.value.owner_special_authority);
         a.top_n_control_flags = 0;
      }
      if( o.extensions.value.active_special_authority.valid() )
      {
         a.active_special_authority = *(o.extensions.value.active_special_authority);
         a.top_n_control_flags = 0;
      }
      sa_after = a.has_special_authority();
   });

   if( sa_before & (!sa_after) )
   {
      const auto& sa_idx = d.get_index_type< special_authority_index >().indices().get<by_account>();
      auto sa_it = sa_idx.find( o.account );
      assert( sa_it != sa_idx.end() );
      d.remove( *sa_it );
   }
   else if( (!sa_before) & sa_after )
   {
      d.create< special_authority_object >( [&]( special_authority_object& sa )
      {
         sa.account = o.account;
      } );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result add_address_evaluator::do_evaluate(const add_address_operation& o) 
{
   database& d = db();

   const auto& idx = d.get_index_type<account_index>().indices().get<by_id>();
   auto itr = idx.find(o.to_account);

   FC_ASSERT(itr != idx.end(), "Account with ID ${id} not exists!", ("a", o.to_account));

   account_ptr = &*itr;

   FC_ASSERT(account_ptr->can_create_addresses, "Account ${a} can't create addresses (restricted by committee)!", ("a", account_ptr->name));

   return void_result();
}

void_result add_address_evaluator::do_apply(const add_address_operation& o)
{
   database& d = db();

   address addr = d.get_address();

   d.modify(*account_ptr, [&](account_object& ao) {
      ao.addresses.emplace_back(addr);
   });
   return void_result();
}

void_result account_whitelist_evaluator::do_evaluate(const account_whitelist_operation& o)
{ try {
   database& d = db();

   listed_account = &o.account_to_list(d);
   if( !d.get_global_properties().parameters.allow_non_member_whitelists )
      FC_ASSERT(o.authorizing_account(d).is_lifetime_member());

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result account_whitelist_evaluator::do_apply(const account_whitelist_operation& o)
{ try {
   database& d = db();

   d.modify(*listed_account, [&o](account_object& a) {
      if( o.new_listing & o.white_listed )
         a.whitelisting_accounts.insert(o.authorizing_account);
      else
         a.whitelisting_accounts.erase(o.authorizing_account);

      if( o.new_listing & o.black_listed )
         a.blacklisting_accounts.insert(o.authorizing_account);
      else
         a.blacklisting_accounts.erase(o.authorizing_account);
   });

   /** for tracking purposes only, this state is not needed to evaluate */
   d.modify( o.authorizing_account(d), [&]( account_object& a ) {
     if( o.new_listing & o.white_listed )
        a.whitelisted_accounts.insert( o.account_to_list );
     else
        a.whitelisted_accounts.erase( o.account_to_list );

     if( o.new_listing & o.black_listed )
        a.blacklisted_accounts.insert( o.account_to_list );
     else
        a.blacklisted_accounts.erase( o.account_to_list );
   });

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result account_upgrade_evaluator::do_evaluate(const account_upgrade_evaluator::operation_type& o)
{ try {
   database& d = db();

   account = &d.get(o.account_to_upgrade);
   FC_ASSERT(!account->is_lifetime_member());

   return {};
//} FC_CAPTURE_AND_RETHROW( (o) ) }
} FC_RETHROW_EXCEPTIONS( error, "Unable to upgrade account '${a}'", ("a",o.account_to_upgrade(db()).name) ) }

void_result account_upgrade_evaluator::do_apply(const account_upgrade_evaluator::operation_type& o)
{ try {
   database& d = db();

   d.modify(*account, [&](account_object& a) {
      if( o.upgrade_to_lifetime_member )
      {
         // Upgrade to lifetime member. I don't care what the account was before.
         a.statistics(d).process_fees(a, d);
         a.membership_expiration_date = time_point_sec::maximum();
         //a.referrer =
         a.registrar = a.lifetime_referrer = a.get_id();
         a.lifetime_referrer_fee_percentage = GRAPHENE_100_PERCENT - a.network_fee_percentage;
      } else if( a.is_annual_member(d.head_block_time()) ) {
         // Renew an annual subscription that's still in effect.
         FC_ASSERT( d.head_block_time() <= HARDFORK_613_TIME );
         FC_ASSERT(a.membership_expiration_date - d.head_block_time() < fc::days(3650),
                   "May not extend annual membership more than a decade into the future.");
         a.membership_expiration_date += fc::days(365);
      } else {
         // Upgrade from basic account.
         FC_ASSERT( d.head_block_time() <= HARDFORK_613_TIME );
         a.statistics(d).process_fees(a, d);
         assert(a.is_basic_account(d.head_block_time()));
         //a.referrer = a.get_id();
         a.membership_expiration_date = d.head_block_time() + fc::days(365);
      }
   });

   return {};
} FC_RETHROW_EXCEPTIONS( error, "Unable to upgrade account '${a}'", ("a",o.account_to_upgrade(db()).name) ) }

void_result account_restrict_evaluator::do_evaluate(const account_restrict_operation& o)
{ try {
    database& d = db();

    const auto& idx = d.get_index_type<restricted_account_index>().indices().get<by_acc_id>();
    auto itr = idx.find(o.target);

    FC_ASSERT( ((o.action & o.restore) && (itr != idx.end())) || (o.action & ~o.restore));

    if (itr != idx.end())
        restricted_account = &*itr;

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

object_id_type account_restrict_evaluator::do_apply(const account_restrict_operation& o)
{ try {
   database& d = db();

   if (o.action & o.restore)
   {
      d.remove( *restricted_account );
   } else
   {
      if (restricted_account == nullptr)
      {
         const auto& new_restr_object =
         d.create< restricted_account_object >( [&]( restricted_account_object& rao )
         {
            rao.account = o.target;
            rao.restriction_type = o.action;
         } );
         return new_restr_object.id;
      }

      d.modify<restricted_account_object> ( *restricted_account,[&]( restricted_account_object& rao)
      {
         rao.restriction_type = o.action;
      } );
   }

   return object_id_type();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result account_allow_create_asset_evaluator::do_evaluate(const allow_create_asset_operation& o)
{ try {
   database& d = db();

   const auto& idx = d.get_index_type<allow_create_asset_account_index>().indices().get<by_acc_id>();
   auto itr = idx.find(o.to_account);

   FC_ASSERT( itr != idx.end() );

   if (itr != idx.end()) {
      allow_create_asset_account = &*itr;
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

object_id_type account_allow_create_asset_evaluator::do_apply(const allow_create_asset_operation& o)
{ try {
   database& d = db();

   if (!o.value){
      d.remove( *allow_create_asset_account );
   }
   else
   {
      if (allow_create_asset_account == nullptr)
      {
         const auto& new_object =
         d.create< allow_create_asset_object>( [&]( allow_create_asset_object& acc )
                                                  {
                                                     acc.account = o.to_account;
                                                     acc.allow = o.value;
                                                  } );
         return new_object.id;
      }

      d.modify<allow_create_asset_object> ( *allow_create_asset_account,[&]( allow_create_asset_object& ao) {
         ao.allow = o.value;
      } );
  }

  return object_id_type();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result account_allow_referrals_evaluator::do_evaluate(const account_allow_referrals_operation& o)
{ try {
   database& d = db();

   auto& p = d.get_account_properties();

   auto itr = p.accounts_properties.find(o.target); 
   FC_ASSERT((itr != p.accounts_properties.end()) || !(o.action & o.disallow) );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

object_id_type account_allow_referrals_evaluator::do_apply(const account_allow_referrals_operation& o)
{ try {
   database& d = db();
   const account_properties_object& _apo = d.get_account_properties();
   d.modify( _apo,[&]( account_properties_object& apo)
   {
      auto itr = apo.accounts_properties.find(o.target); 
      if (itr != apo.accounts_properties.end())
      {
         itr->second.can_be_referrer = o.action & o.allow;
      } else {
         //aaccounts_properties.insert({o.target, {o.action & o.allow}});
         //std::pair p = std::make_pair({o.target, {o.action & o.allow}});
         account_properties_object::account_properties po;
         po.can_be_referrer = o.action & o.allow;
         apo.accounts_properties.insert(std::make_pair(o.target, po));
      }
   } );

   return object_id_type();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result set_online_time_evaluator::do_evaluate(const set_online_time_operation& o)
{ try {
   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result set_online_time_evaluator::do_apply(const set_online_time_operation& o)
{ try {
   database& d = db();
   d.modify(d.get(accounts_online_id_type()), [&](accounts_online_object& obj) {
      obj.online_info = o.online_info;
   });

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result set_verification_is_required_evaluator::do_evaluate(const set_verification_is_required_operation& o)
{ try {
   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result set_verification_is_required_evaluator::do_apply(const set_verification_is_required_operation& o)
{ try {
   database& d = db();
   d.modify(o.target(d), [&](account_object& obj) {
      obj.verification_is_required = o.verification_is_required;
   });

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result allow_create_addresses_evaluator::do_evaluate(const allow_create_addresses_operation& o)
{ try {
   database& d = db();

   const auto& idx = d.get_index_type<account_index>().indices().get<by_id>();
   auto itr = idx.find(o.account_id);

   FC_ASSERT(itr != idx.end(), "Account with ID ${id} not exists!", ("a", o.account_id));

   account_ptr = &*itr;

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result allow_create_addresses_evaluator::do_apply(const allow_create_addresses_operation& o)
{ try {
   database& d = db();

   d.modify<account_object>(*account_ptr, [&](account_object& acc) {
      acc.can_create_addresses = o.allow;
   } );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

} } // graphene::chain
