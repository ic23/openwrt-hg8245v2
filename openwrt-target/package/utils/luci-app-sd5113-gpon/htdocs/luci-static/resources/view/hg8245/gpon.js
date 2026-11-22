'use strict';
'require view';
'require rpc';
'require poll';
'require ui';

function method(name, params) {
	return rpc.declare({ object: 'hg8245.gpon', method: name, params: params });
}
var status = method('status', []), capture = method('omci', ['mode']);

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
		function hasCapture(d) { return !d.managed && d.omci_capture != null; }
		var root = E('div', {}, [
			E('h2', {}, 'GPON'), E('table', { 'class': 'table' }, [
				row('Потеря сигнала (LOS)', 'los'), row('Потеря кадра (LOF)', 'lof'),
				row('Заводской серийный номер', 'factory_serial_status'), row('Назначенный ONU-ID', 'onu_id'),
				row('Уравнивающая задержка, битовые интервалы', 'equalization_delay_bits'),
				row('Получено PLOAM', 'ploam_rx'), row('Ошибки CRC', 'ploam_crc_errors'),
				row('Потеряно PLOAM', 'ploam_dropped'), row('Upstream overhead', 'upstream_overhead'),
				row('VLAN и GEM / Alloc-ID по приоритетам 802.1p', 'gem_service'),
				row('GEM Port-ID (OMCC)', 'omcc_port'), row('Alloc-ID', 'alloc_ids'),
				row('Приём OMCI', 'omci_capture'), row('Получено OMCI', 'omci_received'),
				row('Потеряно из очереди OMCI', 'omci_dropped'), row('Температура оптики, м°C', 'temp1_input'),
				row('Питание оптики, мВ', 'in0_input')
			]),
			button('Начать приём OMCI', function() { return capture('1'); }, hasCapture),
			button('Остановить приём OMCI', function() { return capture('0'); }, hasCapture)
		]);
		update(initial);
		poll.add(refresh, 5);
		return root;
	},
	handleSaveApply: null, handleSave: null, handleReset: null
});
