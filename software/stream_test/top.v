`default_nettype none
`timescale 1ns/1ps

`define GPIO_WIDTH 6
`define USB_DATA_WIDTH 8
`define ADC_DATA_WIDTH 12
`define SD_DATA_WIDTH 4

`include "fifo.v"

module top #(
   // Have the counter cross a clock domain boundary. This tests the
   // transmission quality when the data must pass through a FIFO.
   parameter CROSS_DOMAIN = 0
) (
   // =============== clocks, resets, LEDs, connectors ===============
   // 40MHz
   input wire                              clk_i,
   output wire                             led_o,
   // General-purpose I/O.
   inout wire [`GPIO_WIDTH-1:0]            ext1_io,
   inout wire [`GPIO_WIDTH-1:0]            ext2_io,

   // ==================== FT2232H USB interface. ====================
   // FIFO data
   inout wire signed [`USB_DATA_WIDTH-1:0] ft_data_io,
   // Low when there is data in the buffer that can be read.
   input wire                              ft_rxf_n_i,
   // Low when there is room for transmission data in the FIFO.
   input wire                              ft_txe_n_i,
   // Drive low to load read data to ft_data_io each clock cycle.
   output wire                             ft_rd_n_o,
   // Drive low to write ft_data_io to FIFO for transmission.
   output reg                              ft_wr_n_o,
   // Flush transmission data to USB immediately.
   output wire                             ft_siwua_n_o,
   // 60MHz clock used to synchronize data transfers.
   input wire                              ft_clkout_i,
   // Drive low one period before ft_rd_n_o to signal read.
   output wire                             ft_oe_n_o,
   // Low when USB in suspend mode.
   input wire                              ft_suspend_n_i,

   // ============================== ADC =============================
   // Input data from ADC.
   input wire signed [`ADC_DATA_WIDTH-1:0] adc_d_i,
   // High value indicates overflow or underflow.
   input wire [1:0]                        adc_of_i,
   // LSB refers to channel A, MSB to channel B. Pulling OE and SHDN
   // low enables outputs.  E.g. 10 for each turns on channel A and
   // turns off channel B.
   output reg [1:0]                        adc_oe_o,
   output reg [1:0]                        adc_shdn_o,

   // ============================ SD card ===========================
   inout wire [`SD_DATA_WIDTH-1:0]         sd_data_i,
   inout wire                              sd_cmd_i,
   output reg                              sd_clk_o = 1'b0,
   input wire                              sd_detect_i,

   // ============================= mixer ============================
   // Low voltage enables mixer.
   output reg                              mix_enbl_n_o,

   // ======================== power amplifier =======================
   output wire                             pa_en_n_o,

   // ===================== frequency synthesizer ====================
   output wire                             adf_ce_o,
   output wire                             adf_le_o,
   output wire                             adf_clk_o,
   input wire                              adf_muxout_i,
   output wire                             adf_txdata_o,
   output wire                             adf_data_o,
   // input wire                             adf_done_i,

   // ========================= flash storage ========================
   output reg                              flash_cs_n_o = 1'b1,
   input wire                              flash_miso_i,
   output reg                              flash_mosi_o = 1'b0
);

   assign pa_en_n_o = 1'b1;

   wire                            clk_80mhz;
   wire                            pll_lock;
   wire                            pll_fb;
   PLLE2_BASE #(
      .CLKFBOUT_MULT  (24 ),
      .DIVCLK_DIVIDE  (1  ),
      .CLKOUT0_DIVIDE (12 ),
      .CLKIN1_PERIOD  (25 )
   ) main_pll (
      .CLKOUT0  (clk_80mhz  ),
      .LOCKED   (pll_lock   ),
      .CLKIN1   (clk_i      ),
      .RST      (1'b0       ),
      .CLKFBOUT (pll_fb     ),
      .CLKFBIN  (pll_fb     )
   );
   wire                            rst_n = pll_lock;

   generate
      if (CROSS_DOMAIN == 1) begin
         reg [`USB_DATA_WIDTH-1:0]       ctr;
         wire                            fifo_full;
         wire                            fifo_almost_full;
         wire                            fifo_empty;
         wire                            fifo_almost_empty;
         reg                             fifo_ren;
         wire [`USB_DATA_WIDTH-1:0]      fifo_rdata;
         reg                             fifo_wen;

         assign ext1_io[0] = fifo_full;
         assign ext1_io[1] = fifo_empty;
         assign ext1_io[2] = ft_txe_n_i;

         always @(posedge clk_80mhz) begin
            if (!rst_n) begin
               ctr <= `USB_DATA_WIDTH'd0;
               fifo_wen  <= 1'b0;
            end else begin
               if (!fifo_almost_full) begin
                  ctr <= ctr + 1'b1;
                  fifo_wen <= 1'b1;
               end else begin
                  fifo_wen <= 1'b0;
               end
            end
         end

         fifo #(
            .WIDTH (`USB_DATA_WIDTH ),
            .DEPTH (1024            )
         ) fifo (
            .wclk         (clk_80mhz         ),
            .rst_n        (rst_n             ),
            .wen          (fifo_wen          ),
            .full         (fifo_full         ),
            .almost_full  (fifo_almost_full  ),
            .wdata        (ctr               ),
            .rclk         (ft_clkout_i       ),
            .ren          (fifo_ren          ),
            .empty        (fifo_empty        ),
            .almost_empty (fifo_almost_empty ),
            .rdata        (fifo_rdata        )
         );

         assign ft_data_io = ft_oe_n_o ? fifo_rdata : `USB_DATA_WIDTH'dz;

         always @(*) begin
            ft_wr_n_o = ft_txe_n_i && fifo_ren;
            fifo_ren = ~ft_txe_n_i && ~fifo_almost_empty;
         end

      end else begin
         reg                       ft_txe_last;
         reg [`USB_DATA_WIDTH-1:0] non_cdc_ctr;
         reg [`USB_DATA_WIDTH-1:0] non_cdc_ctr_last;
         assign ft_data_io = ft_oe_n_o ? non_cdc_ctr : `USB_DATA_WIDTH'dz;

         always @(posedge ft_clkout_i) begin
            if (!rst_n) begin
               non_cdc_ctr      <= `USB_DATA_WIDTH'd0;
               non_cdc_ctr_last <= `USB_DATA_WIDTH'd0;
               ft_wr_n_o        <= 1'b1;
               ft_txe_last      <= 1'b0;
            end else begin
               if (!ft_txe_n_i && ft_suspend_n_i) begin
                  ft_wr_n_o        <= 1'b0;
                  non_cdc_ctr      <= non_cdc_ctr + 1'b1;
                  non_cdc_ctr_last <= non_cdc_ctr;
               end else if (ft_txe_n_i && !ft_txe_last) begin
                  non_cdc_ctr <= non_cdc_ctr_last;
                  ft_wr_n_o   <= 1'b1;
               end else begin
                  ft_wr_n_o <= 1'b1;
               end
               ft_txe_last <= ft_txe_n_i;
            end
         end
      end
   endgenerate

   // leave configured for writes
   assign ft_oe_n_o    = 1'b1;
   assign ft_rd_n_o    = 1'b1;
   assign ft_siwua_n_o = 1'b1;

endmodule

`undef GPIO_WIDTH
`undef USB_DATA_WIDTH
`undef ADC_DATA_WIDTH
`undef SD_DATA_WIDTH
