'use strict';
'require view';
'require rpc';
'require poll';
'require ui';

function method(name, params) {
	return rpc.declare({ object: 'hg8245.telephony', method: name, params: params });
}
var status = method('status', []), service = method('service', ['mode']);
var sip = method('sip', ['port', 'peer']);
var line = method('line', ['port', 'mode']);

return view.extend({
	load: function() { return status(); },
	render: function(initial) {
		var cells = {}, buttons = [], writable = L.hasViewPermission();

		function row(label, key) {
			var cell = cells[key] = E('td');
			return E('tr', {}, [E('th', { 'scope': 'row' }, label), cell]);
		}
		function button(label, action, enabled) {
			var b = E('button', { 'class': 'btn', 'type': 'button',
				'click': ui.createHandlerFn(null, function() {
					return action().then(function(r) {
						if (!r.ok) throw new Error('Драйвер отклонил команду');
					}).catch(function(e) {
						ui.addNotification(null, E('p', {}, e.message));
					}).then(refresh);
				})
			}, label);
			buttons.push([b, enabled]);
			return b;
		}
		function update(data) {
			Object.keys(cells).forEach(function(k) {
				cells[k].textContent = data[k] == null ? '—' : String(data[k]);
			});
			buttons.forEach(function(pair) { pair[0].disabled = !writable || !pair[1](data); });
		}
		function refresh() { return status().then(update); }
		function available(d) { return d.service != null && d.sip_state !== 'running'; }
		function free(d) { return d.service === 'on' && d.sip_state !== 'running'; }
		var tel = [row('Сервис', 'service')], actions = [
			button('Включить', function() { return service('on'); }, available),
			button('Интерком', function() { return service('intercom'); }, available),
			button('Выключить', function() { return service('off'); }, available)
		];
		['tel1', 'tel2'].forEach(function(port) {
			var label = port.toUpperCase();
			tel.push(row(label, port), row(label + ' · блоки DSP (10 мс)', port + '_media'));
			actions.push(button('Позвонить на ' + label, function() { return line(port, 'incoming'); }, free));
			actions.push(button('Завершить ' + label, function() { return line(port, 'phone'); }, free));
		});
		var peer = E('input', { type: 'text', value: initial.sip_peer || '', placeholder: 'sip:user@192.168.1.10' });
		var port = E('select', {}, ['tel1', 'tel2'].map(function(k) {
			return E('option', { value: k }, k.toUpperCase());
		}));
		port.value = initial.sip_port || 'tel1';
		var root = E('div', {}, [
			E('h2', {}, 'Телефония'), E('table', { 'class': 'table' }, tel), E('div', {}, actions),
			E('h3', {}, 'SIP через Ethernet'),
			E('table', { 'class': 'table' }, [row('Процесс SIP', 'sip_state'), row('Выбранный порт', 'sip_port')]),
			E('label', {}, ['Телефон ', port]), E('label', {}, [' Адрес вызова при снятии трубки (по желанию) ', peer]),
			button('Применить и включить SIP', function() { return sip(port.value, peer.value.trim()); }, function(d) { return d.service != null; }),
			button('Остановить SIP', function() { return sip('off', ''); }, function() { return true; })
		]);
		update(initial);
		poll.add(refresh, 5);
		return root;
	},
	handleSaveApply: null, handleSave: null, handleReset: null
});
