	/* Run the generated model's code from zero-wait ITCM. The
	   firmware copies it from QSPI flash at startup (bench_main.cpp). The
	   first 8 bytes are skipped so no function sits at address 0. */
	.itcmram_text :
	{
		. = ALIGN(8);
		. = . + 8;
		_itcmram_text_start = .;
		*nam_a2lite*.o(.text .text*)
		*nam_a2full*.o(.text .text*)
		. = ALIGN(4);
		_itcmram_text_end = .;
	} > ITCMRAM AT > QSPIFLASH
	_sitcmram_text = LOADADDR(.itcmram_text) + (_itcmram_text_start - ADDR(.itcmram_text));

