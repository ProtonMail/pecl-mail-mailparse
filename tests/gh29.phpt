--TEST--
GH issue #29 Segmentation fault with ISO-2022-JP Subject header
--SKIPIF--
<?php
if (!extension_loaded("mailparse")) die("skip mailparse extension not available");
?>
--FILE--
<?php

$data = <<<'EOF'
From: aaa@example.jp
To: bbb@example.jp
Subject: $B%X%C%@$"$j(B
Content-type: text/plain; charset=ISO-2022-JP
Content-transfer-encoding: 7bit

$B$*Hh$lMM$G$9!"%F%9%H%a!<%k$G$9!#(B

$B0J>e!"$h$m$7$/$*4j$$CW$7$^$9!#(B
EOF;

$resource = mailparse_msg_create();

$r = mailparse_msg_parse($resource, $data);
echo 'ok', PHP_EOL;

mailparse_msg_free($resource);

exit(0);
?>
--EXPECTF--
ok
