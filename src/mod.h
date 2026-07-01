#pragma once

#include "config.h"

#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bhop::native {
	using RC::Unreal::FBoolProperty;
	using RC::Unreal::FProperty;
	using RC::Unreal::FRotator;
	using RC::Unreal::FVector;
	using RC::Unreal::UClass;
	using RC::Unreal::UFunction;
	using RC::Unreal::UObject;

	inline constexpr std::uint8_t movement_walking                 = 1;
	inline constexpr std::uint8_t movement_falling                 = 3;
	inline constexpr std::uint8_t movement_swimming                = 4;
	inline constexpr std::uint8_t movement_flying                  = 5;
	inline constexpr double       gold_src_crouch_speed_multiplier = 1.0 / 3.0;
	inline constexpr double       mouse_reference_fps              = 120.0;

	using calc_velocity_fn_t          = void ( * )( UObject*, float, float, bool, float );
	using can_crouch_fn_t             = bool ( * )( UObject* );
	using physics_volume_changed_fn_t = void ( * )( UObject*, UObject* );

	extern calc_velocity_fn_t          g_original_calc_velocity;
	extern can_crouch_fn_t             g_original_can_crouch;
	extern physics_volume_changed_fn_t g_original_physics_volume_changed;

	struct movement_properties_t {
		FProperty*     character_owner{};
		FProperty*     velocity{};
		FProperty*     acceleration{};
		FProperty*     max_acceleration{};
		FProperty*     movement_mode{};
		FProperty*     gravity_scale{};
		FProperty*     jump_z_velocity{};
		FProperty*     max_walk_speed{};
		FProperty*     max_walk_speed_crouched{};
		FProperty*     max_sprint_speed{};
		FProperty*     max_swim_speed{};
		FBoolProperty* wants_to_crouch{};

		[[nodiscard]] auto complete( ) const noexcept -> bool;
	};

	struct character_properties_t {
		FBoolProperty* pressed_jump{};
		FProperty*     jump_key_hold_time{};
		FProperty*     controller{};
		FBoolProperty* client_updating{};
		FBoolProperty* was_jumping{};
		FBoolProperty* can_move{};
		FBoolProperty* is_dead{};
		FBoolProperty* is_climbing{};
		FBoolProperty* is_climbing_ladder{};
		FBoolProperty* is_balancing{};
		FBoolProperty* is_falling_balance{};
		FBoolProperty* is_pushing{};
		FBoolProperty* is_crouched{};
		FBoolProperty* should_long_crouch{};
		FBoolProperty* should_scale_crouch{};
		FProperty*     crouch_amount{};
		FBoolProperty* has_water_physics{};
		FBoolProperty* wants_to_crouch_after_landing{};

		[[nodiscard]] auto complete( ) const noexcept -> bool;
	};

	struct movement_state_t {
		bool   overriding{};
		bool   replaying{};
		bool   has_crouch_state{};
		bool   last_wants_crouch{};
		double crouch_hold_seconds{};
		float  gravity_scale{};
		float  jump_z_velocity{};
		float  max_walk_speed{};
		float  max_walk_speed_crouched{};
		float  max_sprint_speed{};
		float  max_swim_speed{};
	};

	struct axis_input_params_t {
		float axis_value{};
	};

	struct ladder_overlap_params_t {
		UObject* overlapped_component{};
		UObject* other_actor{};
	};

	struct ladder_state_t {
		UObject* ladder{};
		vec3_t   start{};
		vec3_t   end{};
		double   half_width_cm{};
		double   contact_depth_cm{};
		bool     on_floor{};
	};

	struct water_state_t {
		bool   submerged{ true };
		double outside_seconds{};
		double outside_limit_seconds{};
	};

	struct interaction_override_t {
		std::size_t  depth{};
		UObject*     movement{};
		std::uint8_t movement_mode{};
		bool         pressed_jump{};
		bool         crouched{};
		bool         wants_to_crouch{};
		bool         should_long_crouch{};
		bool         should_scale_crouch{};
		float        crouch_amount{};
	};

	template < typename T >
	[[nodiscard]] auto property_value( UObject* object, FProperty* property ) noexcept -> T* {
		if ( !object || !property ) {
			return nullptr;
		}
		return static_cast< T* >( property->ContainerPtrToValuePtr< void >( object ) );
	}

	[[nodiscard]] inline auto bool_property( UObject* object, const RC::Unreal::TCHAR* name ) -> FBoolProperty* {
		return object
		    ? RC::Unreal::CastField< FBoolProperty >( object->GetPropertyByNameInChain( name ) )
		    : nullptr;
	}

	[[nodiscard]] inline auto bool_value( UObject* object, FBoolProperty* property, bool fallback = false ) noexcept -> bool {
		return object && property ? property->GetPropertyValueInContainer( object ) : fallback;
	}

	class c_etb_bhop_mod final : public RC::CppUserModBase {
	public:
		c_etb_bhop_mod( );
		~c_etb_bhop_mod( ) override;

		auto               on_unreal_init( ) -> void override;
		auto               handle_calc_velocity( UObject*, float, float, bool, float ) -> void;
		[[nodiscard]] auto should_allow_goldsrc_crouch( UObject* movement ) const -> bool;
		[[nodiscard]] auto begin_water_movement( UObject* movement, UObject* new_volume ) -> bool;

	private:
		auto               call_original( UObject*, float, float, bool, float ) const -> void;
		[[nodiscard]] auto reload_config( ) -> bool;
		auto               apply_mouse_settings( ) -> void;
		auto               restore_mouse_settings( ) -> void;
		auto               cache_properties( UObject* movement, UObject* owner ) -> bool;
		auto               set_movement_mode( UObject* movement, std::uint8_t mode ) const -> bool;

		[[nodiscard]] auto character_is_swimming( UObject* owner ) const -> bool;
		[[nodiscard]] auto character_movement_blocked( UObject* owner ) const -> bool;
		[[nodiscard]] auto movement_is_in_water( UObject* movement, UObject* owner ) -> bool;
		auto               handle_water_velocity( UObject* movement, UObject* owner, double delta_seconds ) -> void;
		auto               handle_water_surface_velocity( UObject* movement, UObject* owner, double delta_seconds ) -> void;
		auto               update_water_state( UObject* owner ) -> void;

		[[nodiscard]] auto read_vector_return( UObject*, UFunction*&, FProperty*&, const RC::Unreal::TCHAR* ) -> std::optional< vec3_t >;
		[[nodiscard]] auto ladder_contains_player( const ladder_state_t&, const vec3_t&, const vec3_t& ) const -> bool;
		auto               detach_from_ladder( UObject* movement, UObject* owner ) -> void;
		[[nodiscard]] auto is_native_transition_ladder( UObject* ladder ) const -> bool;
		[[nodiscard]] auto ladder_normal_for_player( UObject* ladder, UObject* owner ) const -> vec3_t;
		auto               handle_ladder_velocity( UObject*, UObject*, std::uint8_t, ladder_state_t& ) -> void;
		[[nodiscard]] auto activate_ladder( UObject* ladder, UObject* owner, bool require_contact = false ) -> bool;

		auto on_engine_tick( ) -> void;
		auto install_hook( UObject* character ) -> void;
		auto apply_properties( UObject* movement, movement_state_t& state ) -> void;
		auto restore_properties( UObject* movement, movement_state_t& state ) -> void;

		auto register_jump_hooks( ) -> void;
		auto register_crouch_hooks( ) -> void;
		auto register_crouch_event( const RC::StringType& path, int event_id ) -> void;
		auto register_mouse_hooks( ) -> void;
		auto register_movement_input_hooks( ) -> void;
		auto register_interaction_input( ) -> void;
		auto register_interaction_hooks( UObject* character ) -> void;
		auto begin_interaction_override( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void;
		auto end_interaction_override( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void;
		[[nodiscard]] auto try_airborne_interaction( UObject* character ) -> bool;
		auto register_ladder_hook( UObject* ladder ) -> void;
		auto correct_mouse_input( RC::Unreal::UnrealScriptFunctionCallableContext& context ) -> void;
		auto register_commands( ) -> void;

		config_t                                         config_{};
		std::filesystem::path                            config_path_{};
		movement_properties_t                            movement_properties_{};
		character_properties_t                           character_properties_{};
		UClass*                                          character_class_{};
		UFunction*                                       set_movement_mode_function_{};
		FProperty*                                       set_movement_mode_property_{};
		FProperty*                                       mouse_delta_seconds_property_{};
		UFunction*                                       ladder_start_function_{};
		FProperty*                                       ladder_start_return_property_{};
		UFunction*                                       ladder_end_function_{};
		FProperty*                                       ladder_end_return_property_{};
		UFunction*                                       box_extent_function_{};
		FProperty*                                       box_extent_return_property_{};
		UFunction*                                       get_physics_volume_function_{};
		FProperty*                                       get_physics_volume_return_property_{};
		FBoolProperty*                                   water_volume_property_{};
		UObject*                                         input_settings_{};
		FBoolProperty*                                   mouse_smoothing_property_{};
		UFunction*                                       clear_mouse_smoothing_function_{};
		UFunction*                                       ladder_overlap_function_{};
		FProperty*                                       current_interactable_property_{};
		UFunction*                                       interact_function_{};
		FProperty*                                       interact_actor_property_{};
		std::unordered_map< UObject*, movement_state_t > states_{};
		std::unordered_map< UObject*, ladder_state_t >   ladder_states_{};
		std::unordered_map< UObject*, water_state_t >    water_states_{};
		std::unordered_map< UObject*, bool >             jump_held_{};
		std::unordered_map< UObject*, bool >             crouch_held_{};
		std::unordered_map< UObject*, bool >             crouch_release_pending_{};
		std::unordered_map< UObject*, float >            forward_input_{};
		std::unordered_map< UObject*, float >            side_input_{};
		std::unordered_map< UObject*, interaction_override_t > interaction_overrides_{};
		std::optional< int >                             crouch_press_event_{};
		std::vector< std::pair< int, int > >             jump_hook_ids_{};
		std::vector< std::pair< int, int > >             crouch_hook_ids_{};
		std::vector< std::pair< int, int > >             mouse_hook_ids_{};
		std::vector< std::pair< int, int > >             movement_input_hook_ids_{};
		std::vector< std::pair< int, int > >             interaction_hook_ids_{};
		std::vector< std::pair< int, int > >             ladder_hook_ids_{};
		RC::Unreal::Hook::GlobalCallbackId               install_tick_id_{};
		RC::Unreal::Hook::GlobalCallbackId               console_hook_id_{};
		void*                                            hook_target_{};
		void*                                            can_crouch_target_{};
		void*                                            physics_volume_changed_target_{};
		bool                                             install_attempted_{};
		bool                                             hook_ready_{};
		bool                                             minhook_initialized_{};
		bool                                             original_mouse_smoothing_{};
		bool                                             has_original_mouse_smoothing_{};
		bool                                             water_logged_{};
		std::atomic_bool                                 interaction_pressed_{};
		double                                           frame_delta_seconds_{ 1.0 / 60.0 };
	};

	void calc_velocity_detour( UObject*, float, float, bool, float );
	bool can_crouch_detour( UObject* movement );
	void physics_volume_changed_detour( UObject* movement, UObject* new_volume );
}  // namespace bhop::native
