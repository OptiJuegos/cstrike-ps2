/* Phase timers inside the bots' PS2_PERF_BOTS time, from the game library.
 * Printed by PS2_Perf_EndFrame after the PERF line. */
#ifndef PS2_BOT_PERF_H
#define PS2_BOT_PERF_H

enum
{
	PS2_BOT_UPKEEP,  // CCSBot::Upkeep: aiming, 30 Hz
	PS2_BOT_THINK,   // CCSBot::Update, whole full think: includes the next three
	PS2_BOT_CHATTER, //   BotChatterInterface::Update
	PS2_BOT_VISION,  //   UpdateReactionQueue and UpdatePeripheralVision
	PS2_BOT_STATE,   //   the state machine: paths, hunting, attacking
	PS2_BOT_MOVE,    // ExecuteCommand: the player physics of the bot
	PS2_BOT_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned long long ps2_bot_cycles[PS2_BOT_COUNT];
extern unsigned int cpu_ticks( void );
#ifdef __cplusplus
}
#endif

#define PS2_BOT_BEGIN( id ) unsigned int ps2_bot_start_##id = cpu_ticks()
#define PS2_BOT_END( id ) ps2_bot_cycles[PS2_BOT_##id] += cpu_ticks() - ps2_bot_start_##id

#endif // PS2_BOT_PERF_H
