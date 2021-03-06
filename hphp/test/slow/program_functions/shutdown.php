<?php

class X implements SessionHandlerInterface {
  function open($x, $y) {}
  function close() {}
  function destroy($x) {}
  function gc($x) {}
  function read($x) {}
  function write($x, $y) {}
}

function shut() {
  session_set_save_handler(new X, false);
  var_dump(__METHOD__);
}

function baz() {
  var_dump(__METHOD__);
}


<<__EntryPoint>>
function main_shutdown() {
session_set_save_handler(new X, true);
register_shutdown_function("shut");
register_shutdown_function("baz");
}
