// Async: may run at any point. Only its having run is asserted, never when.
document.addEventListener('DOMContentLoaded', function () {
  document.getElementById('async').textContent = 'ASYNC-OK';
});
if (document.readyState !== 'loading') {
  document.getElementById('async').textContent = 'ASYNC-OK';
}
