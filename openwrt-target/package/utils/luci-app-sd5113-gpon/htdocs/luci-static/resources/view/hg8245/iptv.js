'use strict';
'require view';
'require form';
'require tools.widgets as widgets';

return view.extend({
	render: function() {
		var m = new form.Map('igmpproxy', _('IPTV'),
			_('Forward IPv4 multicast streams from the provider to subscribed LAN clients.'));
		var s = m.section(form.TypedSection, 'igmpproxy');
		s.anonymous = true;
		s.option(form.Flag, 'quickleave', _('Quick leave'),
			_('Enable only when one receiver uses the downstream network.'));

		s = m.section(form.TypedSection, 'phyint', _('Multicast interfaces'));
		s.anonymous = true;
		s.addremove = true;
		var o = s.option(form.ListValue, 'direction', _('Direction'));
		o.value('upstream', _('From provider'));
		o.value('downstream', _('To receivers'));
		o.value('disabled', _('Disabled'));
		o = s.option(widgets.NetworkSelect, 'network', _('Network'));
		o.nocreate = true;
		o.rmempty = false;
		o = s.option(widgets.ZoneSelect, 'zone', _('Firewall zone'));
		o.nocreate = true;
		o.rmempty = false;
		o = s.option(form.DynamicList, 'altnet', _('Provider source networks'),
			_('IPv4 prefixes of multicast stream sources. 0.0.0.0/0 accepts any source on the upstream interface.'));
		o.datatype = 'cidr4';
		o.depends('direction', 'upstream');
		return m.render();
	}
});
