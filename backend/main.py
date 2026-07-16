import asyncio
import os
from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from google import genai
from google.genai import types

# Load environment variables
load_dotenv()
GEMINI_API_KEY = os.getenv("GEMINI_API_KEY")

# Initialize FastAPI and Gemini Client
app = FastAPI(title="ESP32 Voice Gateway")
client = genai.Client(api_key=GEMINI_API_KEY)

# Use the official real-time model
MODEL_ID = "gemini-3.1-flash-live-preview" 

# ---------------------------------------------------------
# 1. Define the Custom Tool (The "Brain")
# ---------------------------------------------------------
external_llm_tool = {
    "function_declarations": [
        {
            "name": "consult_external_system",
            "description": "ALWAYS call this tool to answer the user's question or fulfill their request.",
            "parameters": {
                "type": "OBJECT",
                "properties": {
                    "user_query": {
                        "type": "STRING", 
                        "description": "The exact question or request the user made."
                    }
                },
                "required": ["user_query"]
            }
        }
    ]
}

# ---------------------------------------------------------
# 2. Mock External System Logic
# ---------------------------------------------------------
async def call_external_llm(user_query: str) -> str:
    print(f"[SYSTEM] Processing query externally: {user_query}")
    await asyncio.sleep(0.5) 
    return f"I have processed your request regarding: {user_query}. The external system says everything is functioning perfectly."

# ---------------------------------------------------------
# 3. WebSocket Router
# ---------------------------------------------------------
@app.websocket("/ws/voice")
async def voice_agent_endpoint(websocket: WebSocket):
    await websocket.accept()
    print("[SERVER] ESP32 Connected.")
    
    # Configure Gemini for Audio Output and attach the tool
    config = {
        "response_modalities": ["AUDIO"],
        "tools": [external_llm_tool],
        "system_instruction": types.Content(
            parts=[types.Part.from_text(text="You are a voice gateway. You must use the consult_external_system tool to answer the user.")]
        )
    }
    
    try:
        async with client.aio.live.connect(model=MODEL_ID, config=config) as session:
            print("[SERVER] Connected to Gemini Live API.")

            async def stream_mic_to_gemini():
                """Reads binary audio from ESP32 and pushes to Gemini via modern real-time inputs"""
                try:
                    while True:
                        audio_chunk = await websocket.receive_bytes()
                        
                        # Use the protocol-compliant send_realtime_input specifically targeting 'audio'
                        await session.send_realtime_input(
                            audio=types.Blob(
                                data=audio_chunk,
                                mime_type="audio/pcm;rate=24000"
                            )
                        )
                except WebSocketDisconnect:
                    print("[SERVER] ESP32 disconnected.")
                except Exception as e:
                    print(f"[ERROR] Mic stream: {e}")

            async def handle_gemini_responses():
                """Reads responses (Audio or Tool Calls) from Gemini"""
                try:
                    async for response in session.receive():
                        
                        # SCENARIO A: Gemini sent audio to play on the speaker
                        if response.server_content and response.server_content.model_turn:
                            for part in response.server_content.model_turn.parts:
                                if part.inline_data:
                                    # Forward raw 24kHz PCM directly to ESP32
                                    await websocket.send_bytes(part.inline_data.data)

                        # SCENARIO B: Gemini wants to use the external system
                        elif response.tool_call:
                            function_responses = []
                            for fc in response.tool_call.function_calls:
                                if fc.name == "consult_external_system":
                                    query = fc.args["user_query"]
                                    
                                    # 1. Execute your custom logic
                                    result_text = await call_external_llm(query)
                                    
                                    # 2. Package standard FunctionResponse format
                                    function_responses.append(
                                        types.FunctionResponse(
                                            name=fc.name,
                                            id=fc.id,
                                            response={"result": result_text}
                                        )
                                    )
                                    
                            # 3. Use send_tool_response to feed the result securely back to Gemini
                            print("[SERVER] Sending tool result back to Gemini...")
                            await session.send_tool_response(function_responses=function_responses)

                except Exception as e:
                    print(f"[ERROR] Gemini stream: {e}")

            # Run both loops concurrently
            await asyncio.gather(stream_mic_to_gemini(), handle_gemini_responses())

    except Exception as e:
        print(f"[ERROR] Session failed: {e}")
    finally:
        await websocket.close()

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)