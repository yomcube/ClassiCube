﻿using System;
using ClassicalSharp.GraphicsAPI;
using OpenTK.Input;

namespace ClassicalSharp {
	
	public abstract class GuiElement : IDisposable {
		
		protected Game game;
		protected IGraphicsApi graphicsApi;
		
		public GuiElement( Game game ) {
			this.game = game;
			graphicsApi = game.Graphics;
		}
		
		public abstract void Init();
		
		public abstract void Render( double delta );
		
		public abstract void Dispose();
		
		/// <summary> Called when the game window is resized. </summary>
		public abstract void OnResize( int oldWidth, int oldHeight, int width, int height );
		
		public virtual bool HandlesKeyDown( Key key ) {
			return false;
		}
		
		public virtual bool HandlesKeyPress( char key ) {
			return false;
		}
		
		public virtual bool HandlesKeyUp( Key key ) {
			return false;
		}
		
		public virtual bool HandlesMouseClick( int mouseX, int mouseY, MouseButton button ) {
			return false;
		}
		
		public virtual bool HandlesMouseMove( int mouseX, int mouseY ) {
			return false;
		}
		
		public virtual bool HandlesMouseScroll( int delta ) {
			return false;
		}
		
		public virtual bool HandlesMouseUp( int mouseX, int mouseY, MouseButton button ) {
			return false;
		}
		
		protected static int CalcDelta( int newVal, int oldVal, Anchor mode ) {
			return CalcOffset( newVal, oldVal, 0, mode );
		}
		
		protected static int CalcOffset( int axisSize, int elemSize, int offset, Anchor mode ) {
			if( mode == Anchor.LeftOrTop ) return offset;
			if( mode == Anchor.BottomOrRight) return axisSize - elemSize - offset;
			return (axisSize - elemSize) / 2 + offset;
		}
	}
	
	public enum Anchor {
		LeftOrTop = 0,
		Centre = 1,
		BottomOrRight = 2,
	}
}
