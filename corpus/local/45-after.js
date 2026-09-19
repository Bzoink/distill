// Fetched as a blocking script while the page's own requests hold every
// transfer slot: it must wait for one rather than open a ninth connection.
document.getElementById('after').textContent = 'BLOCKING-AFTER-OK';
