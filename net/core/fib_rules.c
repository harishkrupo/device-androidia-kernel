/*
 * net/core/fib_rules.c		Generic Routing Rules
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * Authors:	Thomas Graf <tgraf@suug.ch>
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/module.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/fib_rules.h>
#include <net/ip_tunnels.h>

static const struct fib_kuid_range fib_kuid_range_unset = {
	KUIDT_INIT(0),
	KUIDT_INIT(~0),
};

bool fib_rule_matchall(const struct fib_rule *rule)
{
	if (rule->iifindex || rule->oifindex || rule->mark || rule->tun_id ||
	    rule->flags)
		return false;
	if (rule->suppress_ifgroup != -1 || rule->suppress_prefixlen != -1)
		return false;
	if (!uid_eq(rule->uid_range.start, fib_kuid_range_unset.start) ||
	    !uid_eq(rule->uid_range.end, fib_kuid_range_unset.end))
		return false;
	return true;
}
EXPORT_SYMBOL_GPL(fib_rule_matchall);

int fib_default_rule_add(struct fib_rules_ops *ops,
			 u32 pref, u32 table, u32 flags)
{
	struct fib_rule *r;

	r = kzalloc(ops->rule_size, GFP_KERNEL);
	if (r == NULL)
		return -ENOMEM;

	refcount_set(&r->refcnt, 1);
	r->action = FR_ACT_TO_TBL;
	r->pref = pref;
	r->table = table;
	r->flags = flags;
	r->fr_net = ops->fro_net;
	r->uid_range = fib_kuid_range_unset;

	r->suppress_prefixlen = -1;
	r->suppress_ifgroup = -1;

	/* The lock is not required here, the list in unreacheable
	 * at the moment this function is called */
	list_add_tail(&r->list, &ops->rules_list);
	return 0;
}
EXPORT_SYMBOL(fib_default_rule_add);

static u32 fib_default_rule_pref(struct fib_rules_ops *ops)
{
	struct list_head *pos;
	struct fib_rule *rule;

	if (!list_empty(&ops->rules_list)) {
		pos = ops->rules_list.next;
		if (pos->next != &ops->rules_list) {
			rule = list_entry(pos->next, struct fib_rule, list);
			if (rule->pref)
				return rule->pref - 1;
		}
	}

	return 0;
}

static void notify_rule_change(int event, struct fib_rule *rule,
			       struct fib_rules_ops *ops, struct nlmsghdr *nlh,
			       u32 pid);

static struct fib_rules_ops *lookup_rules_ops(struct net *net, int family)
{
	struct fib_rules_ops *ops;

	rcu_read_lock();
	list_for_each_entry_rcu(ops, &net->rules_ops, list) {
		if (ops->family == family) {
			if (!try_module_get(ops->owner))
				ops = NULL;
			rcu_read_unlock();
			return ops;
		}
	}
	rcu_read_unlock();

	return NULL;
}

static void rules_ops_put(struct fib_rules_ops *ops)
{
	if (ops)
		module_put(ops->owner);
}

static void flush_route_cache(struct fib_rules_ops *ops)
{
	if (ops->flush_cache)
		ops->flush_cache(ops);
}

static int __fib_rules_register(struct fib_rules_ops *ops)
{
	int err = -EEXIST;
	struct fib_rules_ops *o;
	struct net *net;

	net = ops->fro_net;

	if (ops->rule_size < sizeof(struct fib_rule))
		return -EINVAL;

	if (ops->match == NULL || ops->configure == NULL ||
	    ops->compare == NULL || ops->fill == NULL ||
	    ops->action == NULL)
		return -EINVAL;

	spin_lock(&net->rules_mod_lock);
	list_for_each_entry(o, &net->rules_ops, list)
		if (ops->family == o->family)
			goto errout;

	list_add_tail_rcu(&ops->list, &net->rules_ops);
	err = 0;
errout:
	spin_unlock(&net->rules_mod_lock);

	return err;
}

struct fib_rules_ops *
fib_rules_register(const struct fib_rules_ops *tmpl, struct net *net)
{
	struct fib_rules_ops *ops;
	int err;

	ops = kmemdup(tmpl, sizeof(*ops), GFP_KERNEL);
	if (ops == NULL)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->rules_list);
	ops->fro_net = net;

	err = __fib_rules_register(ops);
	if (err) {
		kfree(ops);
		ops = ERR_PTR(err);
	}

	return ops;
}
EXPORT_SYMBOL_GPL(fib_rules_register);

static void fib_rules_cleanup_ops(struct fib_rules_ops *ops)
{
	struct fib_rule *rule, *tmp;

	list_for_each_entry_safe(rule, tmp, &ops->rules_list, list) {
		list_del_rcu(&rule->list);
		if (ops->delete)
			ops->delete(rule);
		fib_rule_put(rule);
	}
}

void fib_rules_unregister(struct fib_rules_ops *ops)
{
	struct net *net = ops->fro_net;

	spin_lock(&net->rules_mod_lock);
	list_del_rcu(&ops->list);
	spin_unlock(&net->rules_mod_lock);

	fib_rules_cleanup_ops(ops);
	kfree_rcu(ops, rcu);
}
EXPORT_SYMBOL_GPL(fib_rules_unregister);

static int uid_range_set(struct fib_kuid_range *range)
{
	return uid_valid(range->start) && uid_valid(range->end);
}

static struct fib_kuid_range nla_get_kuid_range(struct nlattr **tb)
{
	struct fib_rule_uid_range *in;
	struct fib_kuid_range out;

	in = (struct fib_rule_uid_range *)nla_data(tb[FRA_UID_RANGE]);

	out.start = make_kuid(current_user_ns(), in->start);
	out.end = make_kuid(current_user_ns(), in->end);

	return out;
}

static int nla_put_uid_range(struct sk_buff *skb, struct fib_kuid_range *range)
{
	struct fib_rule_uid_range out = {
		from_kuid_munged(current_user_ns(), range->start),
		from_kuid_munged(current_user_ns(), range->end)
	};

	return nla_put(skb, FRA_UID_RANGE, sizeof(out), &out);
}

static int fib_rule_match(struct fib_rule *rule, struct fib_rules_ops *ops,
			  struct flowi *fl, int flags,
			  struct fib_lookup_arg *arg)
{
	int ret = 0;

	if (rule->iifindex && (rule->iifindex != fl->flowi_iif))
		goto out;

	if (rule->oifindex && (rule->oifindex != fl->flowi_oif))
		goto out;

	if ((rule->mark ^ fl->flowi_mark) & rule->mark_mask)
		goto out;

	if (rule->tun_id && (rule->tun_id != fl->flowi_tun_key.tun_id))
		goto out;

	if (rule->l3mdev && !l3mdev_fib_rule_match(rule->fr_net, fl, arg))
		goto out;

	if (uid_lt(fl->flowi_uid, rule->uid_range.start) ||
	    uid_gt(fl->flowi_uid, rule->uid_range.end))
		goto out;

	ret = ops->match(rule, fl, flags);
out:
	return (rule->flags & FIB_RULE_INVERT) ? !ret : ret;
}

int fib_rules_lookup(struct fib_rules_ops *ops, struct flowi *fl,
		     int flags, struct fib_lookup_arg *arg)
{
	struct fib_rule *rule;
	int err;

	rcu_read_lock();

	list_for_each_entry_rcu(rule, &ops->rules_list, list) {
jumped:
		if (!fib_rule_match(rule, ops, fl, flags, arg))
			continue;

		if (rule->action == FR_ACT_GOTO) {
			struct fib_rule *target;

			target = rcu_dereference(rule->ctarget);
			if (target == NULL) {
				continue;
			} else {
				rule = target;
				goto jumped;
			}
		} else if (rule->action == FR_ACT_NOP)
			continue;
		else
			err = ops->action(rule, fl, flags, arg);

		if (!err && ops->suppress && ops->suppress(rule, arg))
			continue;

		if (err != -EAGAIN) {
			if ((arg->flags & FIB_LOOKUP_NOREF) ||
			    likely(refcount_inc_not_zero(&rule->refcnt))) {
				arg->rule = rule;
				goto out;
			}
			break;
		}
	}

	err = -ESRCH;
out:
	rcu_read_unlock();

	return err;
}
EXPORT_SYMBOL_GPL(fib_rules_lookup);

static int call_fib_rule_notifier(struct notifier_block *nb, struct net *net,
				  enum fib_event_type event_type,
				  struct fib_rule *rule, int family)
{
	struct fib_rule_notifier_info info = {
		.info.family = family,
		.rule = rule,
	};

	return call_fib_notifier(nb, net, event_type, &info.info);
}

static int call_fib_rule_notifiers(struct net *net,
				   enum fib_event_type event_type,
				   struct fib_rule *rule,
				   struct fib_rules_ops *ops)
{
	struct fib_rule_notifier_info info = {
		.info.family = ops->family,
		.rule = rule,
	};

	ops->fib_rules_seq++;
	return call_fib_notifiers(net, event_type, &info.info);
}

/* Called with rcu_read_lock() */
int fib_rules_dump(struct net *net, struct notifier_block *nb, int family)
{
	struct fib_rules_ops *ops;
	struct fib_rule *rule;

	ops = lookup_rules_ops(net, family);
	if (!ops)
		return -EAFNOSUPPORT;
	list_for_each_entry_rcu(rule, &ops->rules_list, list)
		call_fib_rule_notifier(nb, net, FIB_EVENT_RULE_ADD, rule,
				       family);
	rules_ops_put(ops);

	return 0;
}
EXPORT_SYMBOL_GPL(fib_rules_dump);

unsigned int fib_rules_seq_read(struct net *net, int family)
{
	unsigned int fib_rules_seq;
	struct fib_rules_ops *ops;

	ASSERT_RTNL();

	ops = lookup_rules_ops(net, family);
	if (!ops)
		return 0;
	fib_rules_seq = ops->fib_rules_seq;
	rules_ops_put(ops);

	return fib_rules_seq;
}
EXPORT_SYMBOL_GPL(fib_rules_seq_read);

static int validate_rulemsg(struct fib_rule_hdr *frh, struct nlattr **tb,
			    struct fib_rules_ops *ops)
{
	int err = -EINVAL;

	if (frh->src_len)
		if (tb[FRA_SRC] == NULL ||
		    frh->src_len > (ops->addr_size * 8) ||
		    nla_len(tb[FRA_SRC]) != ops->addr_size)
			goto errout;

	if (frh->dst_len)
		if (tb[FRA_DST] == NULL ||
		    frh->dst_len > (ops->addr_size * 8) ||
		    nla_len(tb[FRA_DST]) != ops->addr_size)
			goto errout;

	err = 0;
errout:
	return err;
}

static int rule_exists(struct fib_rules_ops *ops, struct fib_rule_hdr *frh,
		       struct nlattr **tb, struct fib_rule *rule)
{
	struct fib_rule *r;

	list_for_each_entry(r, &ops->rules_list, list) {
		if (r->action != rule->action)
			continue;

		if (r->table != rule->table)
			continue;

		if (r->pref != rule->pref)
			continue;

		if (memcmp(r->iifname, rule->iifname, IFNAMSIZ))
			continue;

		if (memcmp(r->oifname, rule->oifname, IFNAMSIZ))
			continue;

		if (r->mark != rule->mark)
			continue;

		if (r->mark_mask != rule->mark_mask)
			continue;

		if (r->tun_id != rule->tun_id)
			continue;

		if (r->fr_net != rule->fr_net)
			continue;

		if (r->l3mdev != rule->l3mdev)
			continue;

		if (!uid_eq(r->uid_range.start, rule->uid_range.start) ||
		    !uid_eq(r->uid_range.end, rule->uid_range.end))
			continue;

		if (!ops->compare(r, frh, tb))
			continue;
		return 1;
	}
	return 0;
}

int fib_nl_newrule(struct sk_buff *skb, struct nlmsghdr *nlh,
		   struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct fib_rule_hdr *frh = nlmsg_data(nlh);
	struct fib_rules_ops *ops = NULL;
	struct fib_rule *rule, *r, *last = NULL;
	struct nlattr *tb[FRA_MAX+1];
	int err = -EINVAL, unresolved = 0;

	if (nlh->nlmsg_len < nlmsg_msg_size(sizeof(*frh)))
		goto errout;

	ops = lookup_rules_ops(net, frh->family);
	if (ops == NULL) {
		err = -EAFNOSUPPORT;
		goto errout;
	}

	err = nlmsg_parse(nlh, sizeof(*frh), tb, FRA_MAX, ops->policy, extack);
	if (err < 0)
		goto errout;

	err = validate_rulemsg(frh, tb, ops);
	if (err < 0)
		goto errout;

	rule = kzalloc(ops->rule_size, GFP_KERNEL);
	if (rule == NULL) {
		err = -ENOMEM;
		goto errout;
	}
	refcount_set(&rule->refcnt, 1);
	rule->fr_net = net;

	rule->pref = tb[FRA_PRIORITY] ? nla_get_u32(tb[FRA_PRIORITY])
	                              : fib_default_rule_pref(ops);

	if (tb[FRA_IIFNAME]) {
		struct net_device *dev;

		rule->iifindex = -1;
		nla_strlcpy(rule->iifname, tb[FRA_IIFNAME], IFNAMSIZ);
		dev = __dev_get_by_name(net, rule->iifname);
		if (dev)
			rule->iifindex = dev->ifindex;
	}

	if (tb[FRA_OIFNAME]) {
		struct net_device *dev;

		rule->oifindex = -1;
		nla_strlcpy(rule->oifname, tb[FRA_OIFNAME], IFNAMSIZ);
		dev = __dev_get_by_name(net, rule->oifname);
		if (dev)
			rule->oifindex = dev->ifindex;
	}

	if (tb[FRA_FWMARK]) {
		rule->mark = nla_get_u32(tb[FRA_FWMARK]);
		if (rule->mark)
			/* compatibility: if the mark value is non-zero all bits
			 * are compared unless a mask is explicitly specified.
			 */
			rule->mark_mask = 0xFFFFFFFF;
	}

	if (tb[FRA_FWMASK])
		rule->mark_mask = nla_get_u32(tb[FRA_FWMASK]);

	if (tb[FRA_TUN_ID])
		rule->tun_id = nla_get_be64(tb[FRA_TUN_ID]);

	err = -EINVAL;
	if (tb[FRA_L3MDEV]) {
#ifdef CONFIG_NET_L3_MASTER_DEV
		rule->l3mdev = nla_get_u8(tb[FRA_L3MDEV]);
		if (rule->l3mdev != 1)
#endif
			goto errout_free;
	}

	rule->action = frh->action;
	rule->flags = frh->flags;
	rule->table = frh_get_table(frh, tb);
	if (tb[FRA_SUPPRESS_PREFIXLEN])
		rule->suppress_prefixlen = nla_get_u32(tb[FRA_SUPPRESS_PREFIXLEN]);
	else
		rule->suppress_prefixlen = -1;

	if (tb[FRA_SUPPRESS_IFGROUP])
		rule->suppress_ifgroup = nla_get_u32(tb[FRA_SUPPRESS_IFGROUP]);
	else
		rule->suppress_ifgroup = -1;

	if (tb[FRA_GOTO]) {
		if (rule->action != FR_ACT_GOTO)
			goto errout_free;

		rule->target = nla_get_u32(tb[FRA_GOTO]);
		/* Backward jumps are prohibited to avoid endless loops */
		if (rule->target <= rule->pref)
			goto errout_free;

		list_for_each_entry(r, &ops->rules_list, list) {
			if (r->pref == rule->target) {
				RCU_INIT_POINTER(rule->ctarget, r);
				break;
			}
		}

		if (rcu_dereference_protected(rule->ctarget, 1) == NULL)
			unresolved = 1;
	} else if (rule->action == FR_ACT_GOTO)
		goto errout_free;

	if (rule->l3mdev && rule->table)
		goto errout_free;

	if (tb[FRA_UID_START] && tb[FRA_UID_END]) {
		if (current_user_ns() != net->user_ns) {
			err = -EPERM;
			goto errout_free;
		}

		rule->uid_range.start = make_kuid(current_user_ns(),
						nla_get_u32(tb[FRA_UID_START]));
		rule->uid_range.end = make_kuid(current_user_ns(),
						nla_get_u32(tb[FRA_UID_END]));

		if (!uid_range_set(&rule->uid_range) ||
				!uid_lte(rule->uid_range.start, rule->uid_range.end))
			goto errout_free;
	} else {
		rule->uid_range = fib_kuid_range_unset;
	}

	if (tb[FRA_UID_RANGE]) {
		if (current_user_ns() != net->user_ns) {
			err = -EPERM;
			goto errout_free;
		}

		rule->uid_range = nla_get_kuid_range(tb);

		if (!uid_range_set(&rule->uid_range) ||
		    !uid_lte(rule->uid_range.start, rule->uid_range.end))
			goto errout_free;
	} else {
		rule->uid_range = fib_kuid_range_unset;
	}

	if ((nlh->nlmsg_flags & NLM_F_EXCL) &&
	    rule_exists(ops, frh, tb, rule)) {
		err = -EEXIST;
		goto errout_free;
	}

	err = ops->configure(rule, skb, frh, tb);
	if (err < 0)
		goto errout_free;

	list_for_each_entry(r, &ops->rules_list, list) {
		if (r->pref > rule->pref)
			break;
		last = r;
	}

	if (last)
		list_add_rcu(&rule->list, &last->list);
	else
		list_add_rcu(&rule->list, &ops->rules_list);

	if (ops->unresolved_rules) {
		/*
		 * There are unresolved goto rules in the list, check if
		 * any of them are pointing to this new rule.
		 */
		list_for_each_entry(r, &ops->rules_list, list) {
			if (r->action == FR_ACT_GOTO &&
			    r->target == rule->pref &&
			    rtnl_dereference(r->ctarget) == NULL) {
				rcu_assign_pointer(r->ctarget, rule);
				if (--ops->unresolved_rules == 0)
					break;
			}
		}
	}

	if (rule->action == FR_ACT_GOTO)
		ops->nr_goto_rules++;

	if (unresolved)
		ops->unresolved_rules++;

	if (rule->tun_id)
		ip_tunnel_need_metadata();

	call_fib_rule_notifiers(net, FIB_EVENT_RULE_ADD, rule, ops);
	notify_rule_change(RTM_NEWRULE, rule, ops, nlh, NETLINK_CB(skb).portid);
	flush_route_cache(ops);
	rules_ops_put(ops);
	return 0;

errout_free:
	kfree(rule);
errout:
	rules_ops_put(ops);
	return err;
}
EXPORT_SYMBOL_GPL(fib_nl_newrule);

int fib_nl_delrule(struct sk_buff *skb, struct nlmsghdr *nlh,
		   struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct fib_rule_hdr *frh = nlmsg_data(nlh);
	struct fib_rules_ops *ops = NULL;
	struct fib_rule *rule, *r;
	struct nlattr *tb[FRA_MAX+1];
	struct fib_kuid_range range;
	int err = -EINVAL;

	if (nlh->nlmsg_len < nlmsg_msg_size(sizeof(*frh)))
		goto errout;

	ops = lookup_rules_ops(net, frh->family);
	if (ops == NULL) {
		err = -EAFNOSUPPORT;
		goto errout;
	}

	err = nlmsg_parse(nlh, sizeof(*frh), tb, FRA_MAX, ops->policy, extack);
	if (err < 0)
		goto errout;

	err = validate_rulemsg(frh, tb, ops);
	if (err < 0)
		goto errout;

	if (tb[FRA_UID_START] && tb[FRA_UID_END]) {
		range.start = make_kuid(current_user_ns(),
					nla_get_u32(tb[FRA_UID_START]));
		range.end = make_kuid(current_user_ns(),
					nla_get_u32(tb[FRA_UID_END]));

		if (!uid_range_set(&range))
			goto errout;
	} else {
		range = fib_kuid_range_unset;
	}

	if (tb[FRA_UID_RANGE]) {
		range = nla_get_kuid_range(tb);
		if (!uid_range_set(&range)) {
			err = -EINVAL;
			goto errout;
		}
	} else {
		range = fib_kuid_range_unset;
	}

	list_for_each_entry(rule, &ops->rules_list, list) {
		if (frh->action && (frh->action != rule->action))
			continue;

		if (frh_get_table(frh, tb) &&
		    (frh_get_table(frh, tb) != rule->table))
			continue;

		if (tb[FRA_PRIORITY] &&
		    (rule->pref != nla_get_u32(tb[FRA_PRIORITY])))
			continue;

		if (tb[FRA_IIFNAME] &&
		    nla_strcmp(tb[FRA_IIFNAME], rule->iifname))
			continue;

		if (tb[FRA_OIFNAME] &&
		    nla_strcmp(tb[FRA_OIFNAME], rule->oifname))
			continue;

		if (tb[FRA_FWMARK] &&
		    (rule->mark != nla_get_u32(tb[FRA_FWMARK])))
			continue;

		if (tb[FRA_FWMASK] &&
		    (rule->mark_mask != nla_get_u32(tb[FRA_FWMASK])))
			continue;

		if (tb[FRA_TUN_ID] &&
		    (rule->tun_id != nla_get_be64(tb[FRA_TUN_ID])))
			continue;

		if (tb[FRA_L3MDEV] &&
		    (rule->l3mdev != nla_get_u8(tb[FRA_L3MDEV])))
			continue;

		if (uid_range_set(&range) &&
		    (!uid_eq(rule->uid_range.start, range.start) ||
		     !uid_eq(rule->uid_range.end, range.end)))
			continue;

		if (!ops->compare(rule, frh, tb))
			continue;

		if (rule->flags & FIB_RULE_PERMANENT) {
			err = -EPERM;
			goto errout;
		}

		if (ops->delete) {
			err = ops->delete(rule);
			if (err)
				goto errout;
		}

		if (rule->tun_id)
			ip_tunnel_unneed_metadata();

		list_del_rcu(&rule->list);

		if (rule->action == FR_ACT_GOTO) {
			ops->nr_goto_rules--;
			if (rtnl_dereference(rule->ctarget) == NULL)
				ops->unresolved_rules--;
		}

		/*
		 * Check if this rule is a target to any of them. If so,
		 * adjust to the next one with the same preference or
		 * disable them. As this operation is eventually very
		 * expensive, it is only performed if goto rules, except
		 * current if it is goto rule, have actually been added.
		 */
		if (ops->nr_goto_rules > 0) {
			struct fib_rule *n;

			n = list_next_entry(rule, list);
			if (&n->list == &ops->rules_list || n->pref != rule->pref)
				n = NULL;
			list_for_each_entry(r, &ops->rules_list, list) {
				if (rtnl_dereference(r->ctarget) != rule)
					continue;
				rcu_assign_pointer(r->ctarget, n);
				if (!n)
					ops->unresolved_rules++;
			}
		}

		call_fib_rule_notifiers(net, FIB_EVENT_RULE_DEL, rule, ops);
		notify_rule_change(RTM_DELRULE, rule, ops, nlh,
				   NETLINK_CB(skb).portid);
		fib_rule_put(rule);
		flush_route_cache(ops);
		rules_ops_put(ops);
		return 0;
	}

	err = -ENOENT;
errout:
	rules_ops_put(ops);
	return err;
}
EXPORT_SYMBOL_GPL(fib_nl_delrule);

static inline size_t fib_rule_nlmsg_size(struct fib_rules_ops *ops,
					 struct fib_rule *rule)
{
	size_t payload = NLMSG_ALIGN(sizeof(struct fib_rule_hdr))
			 + nla_total_size(IFNAMSIZ) /* FRA_IIFNAME */
			 + nla_total_size(IFNAMSIZ) /* FRA_OIFNAME */
			 + nla_total_size(4) /* FRA_PRIORITY */
			 + nla_total_size(4) /* FRA_TABLE */
			 + nla_total_size(4) /* FRA_SUPPRESS_PREFIXLEN */
			 + nla_total_size(4) /* FRA_SUPPRESS_IFGROUP */
			 + nla_total_size(4) /* FRA_FWMARK */
			 + nla_total_size(4) /* FRA_FWMASK */
			 + nla_total_size_64bit(8) /* FRA_TUN_ID */
			 + nla_total_size(sizeof(struct fib_kuid_range));

	if (ops->nlmsg_payload)
		payload += ops->nlmsg_payload(rule);

	return payload;
}

static int fib_nl_fill_rule(struct sk_buff *skb, struct fib_rule *rule,
			    u32 pid, u32 seq, int type, int flags,
			    struct fib_rules_ops *ops)
{
	struct nlmsghdr *nlh;
	struct fib_rule_hdr *frh;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*frh), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	frh = nlmsg_data(nlh);
	frh->family = ops->family;
	frh->table = rule->table;
	if (nla_put_u32(skb, FRA_TABLE, rule->table))
		goto nla_put_failure;
	if (nla_put_u32(skb, FRA_SUPPRESS_PREFIXLEN, rule->suppress_prefixlen))
		goto nla_put_failure;
	frh->res1 = 0;
	frh->res2 = 0;
	frh->action = rule->action;
	frh->flags = rule->flags;

	if (rule->action == FR_ACT_GOTO &&
	    rcu_access_pointer(rule->ctarget) == NULL)
		frh->flags |= FIB_RULE_UNRESOLVED;

	if (rule->iifname[0]) {
		if (nla_put_string(skb, FRA_IIFNAME, rule->iifname))
			goto nla_put_failure;
		if (rule->iifindex == -1)
			frh->flags |= FIB_RULE_IIF_DETACHED;
	}

	if (rule->oifname[0]) {
		if (nla_put_string(skb, FRA_OIFNAME, rule->oifname))
			goto nla_put_failure;
		if (rule->oifindex == -1)
			frh->flags |= FIB_RULE_OIF_DETACHED;
	}

	if ((rule->pref &&
	     nla_put_u32(skb, FRA_PRIORITY, rule->pref)) ||
	    (rule->mark &&
	     nla_put_u32(skb, FRA_FWMARK, rule->mark)) ||
	    ((rule->mark_mask || rule->mark) &&
	     nla_put_u32(skb, FRA_FWMASK, rule->mark_mask)) ||
	    (rule->target &&
	     nla_put_u32(skb, FRA_GOTO, rule->target)) ||
	    (rule->tun_id &&
	     nla_put_be64(skb, FRA_TUN_ID, rule->tun_id, FRA_PAD)) ||
	    (rule->l3mdev &&
	     nla_put_u8(skb, FRA_L3MDEV, rule->l3mdev)) ||
	    (uid_range_set(&rule->uid_range) &&
	     nla_put_uid_range(skb, &rule->uid_range)))
		goto nla_put_failure;

	if (rule->suppress_ifgroup != -1) {
		if (nla_put_u32(skb, FRA_SUPPRESS_IFGROUP, rule->suppress_ifgroup))
			goto nla_put_failure;
	}

	if (ops->fill(rule, skb, frh) < 0)
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static int dump_rules(struct sk_buff *skb, struct netlink_callback *cb,
		      struct fib_rules_ops *ops)
{
	int idx = 0;
	struct fib_rule *rule;
	int err = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(rule, &ops->rules_list, list) {
		if (idx < cb->args[1])
			goto skip;

		err = fib_nl_fill_rule(skb, rule, NETLINK_CB(cb->skb).portid,
				       cb->nlh->nlmsg_seq, RTM_NEWRULE,
				       NLM_F_MULTI, ops);
		if (err)
			break;
skip:
		idx++;
	}
	rcu_read_unlock();
	cb->args[1] = idx;
	rules_ops_put(ops);

	return err;
}

static int fib_nl_dumprule(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct fib_rules_ops *ops;
	int idx = 0, family;

	family = rtnl_msg_family(cb->nlh);
	if (family != AF_UNSPEC) {
		/* Protocol specific dump request */
		ops = lookup_rules_ops(net, family);
		if (ops == NULL)
			return -EAFNOSUPPORT;

		dump_rules(skb, cb, ops);

		return skb->len;
	}

	rcu_read_lock();
	list_for_each_entry_rcu(ops, &net->rules_ops, list) {
		if (idx < cb->args[0] || !try_module_get(ops->owner))
			goto skip;

		if (dump_rules(skb, cb, ops) < 0)
			break;

		cb->args[1] = 0;
skip:
		idx++;
	}
	rcu_read_unlock();
	cb->args[0] = idx;

	return skb->len;
}

static void notify_rule_change(int event, struct fib_rule *rule,
			       struct fib_rules_ops *ops, struct nlmsghdr *nlh,
			       u32 pid)
{
	struct net *net;
	struct sk_buff *skb;
	int err = -ENOBUFS;

	net = ops->fro_net;
	skb = nlmsg_new(fib_rule_nlmsg_size(ops, rule), GFP_KERNEL);
	if (skb == NULL)
		goto errout;

	err = fib_nl_fill_rule(skb, rule, pid, nlh->nlmsg_seq, event, 0, ops);
	if (err < 0) {
		/* -EMSGSIZE implies BUG in fib_rule_nlmsg_size() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}

	rtnl_notify(skb, net, pid, ops->nlgroup, nlh, GFP_KERNEL);
	return;
errout:
	if (err < 0)
		rtnl_set_sk_err(net, ops->nlgroup, err);
}

static void attach_rules(struct list_head *rules, struct net_device *dev)
{
	struct fib_rule *rule;

	list_for_each_entry(rule, rules, list) {
		if (rule->iifindex == -1 &&
		    strcmp(dev->name, rule->iifname) == 0)
			rule->iifindex = dev->ifindex;
		if (rule->oifindex == -1 &&
		    strcmp(dev->name, rule->oifname) == 0)
			rule->oifindex = dev->ifindex;
	}
}

static void detach_rules(struct list_head *rules, struct net_device *dev)
{
	struct fib_rule *rule;

	list_for_each_entry(rule, rules, list) {
		if (rule->iifindex == dev->ifindex)
			rule->iifindex = -1;
		if (rule->oifindex == dev->ifindex)
			rule->oifindex = -1;
	}
}


static int fib_rules_event(struct notifier_block *this, unsigned long event,
			   void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct net *net = dev_net(dev);
	struct fib_rules_ops *ops;

	ASSERT_RTNL();

	switch (event) {
	case NETDEV_REGISTER:
		list_for_each_entry(ops, &net->rules_ops, list)
			attach_rules(&ops->rules_list, dev);
		break;

	case NETDEV_CHANGENAME:
		list_for_each_entry(ops, &net->rules_ops, list) {
			detach_rules(&ops->rules_list, dev);
			attach_rules(&ops->rules_list, dev);
		}
		break;

	case NETDEV_UNREGISTER:
		list_for_each_entry(ops, &net->rules_ops, list)
			detach_rules(&ops->rules_list, dev);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block fib_rules_notifier = {
	.notifier_call = fib_rules_event,
};

static int __net_init fib_rules_net_init(struct net *net)
{
	INIT_LIST_HEAD(&net->rules_ops);
	spin_lock_init(&net->rules_mod_lock);
	return 0;
}

static struct pernet_operations fib_rules_net_ops = {
	.init = fib_rules_net_init,
};

static int __init fib_rules_init(void)
{
	int err;
	rtnl_register(PF_UNSPEC, RTM_NEWRULE, fib_nl_newrule, NULL, 0);
	rtnl_register(PF_UNSPEC, RTM_DELRULE, fib_nl_delrule, NULL, 0);
	rtnl_register(PF_UNSPEC, RTM_GETRULE, NULL, fib_nl_dumprule, 0);

	err = register_pernet_subsys(&fib_rules_net_ops);
	if (err < 0)
		goto fail;

	err = register_netdevice_notifier(&fib_rules_notifier);
	if (err < 0)
		goto fail_unregister;

	return 0;

fail_unregister:
	unregister_pernet_subsys(&fib_rules_net_ops);
fail:
	rtnl_unregister(PF_UNSPEC, RTM_NEWRULE);
	rtnl_unregister(PF_UNSPEC, RTM_DELRULE);
	rtnl_unregister(PF_UNSPEC, RTM_GETRULE);
	return err;
}

subsys_initcall(fib_rules_init);
