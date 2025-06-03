import React, { useState, useEffect, useRef } from 'react';
import './App.css';
import axios from 'axios';

const BACKEND_URL = process.env.REACT_APP_BACKEND_URL;
const API = `${BACKEND_URL}/api`;

// Window Management Component
const Window = ({ 
  id, 
  title, 
  children, 
  onClose, 
  onMinimize, 
  onMaximize, 
  isMaximized = false,
  initialPosition = { x: 100, y: 100 },
  initialSize = { width: 600, height: 400 },
  zIndex = 1,
  onFocus
}) => {
  const [position, setPosition] = useState(initialPosition);
  const [size, setSize] = useState(initialSize);
  const [isDragging, setIsDragging] = useState(false);
  const [isResizing, setIsResizing] = useState(false);
  const [resizeDirection, setResizeDirection] = useState('');
  const [dragOffset, setDragOffset] = useState({ x: 0, y: 0 });
  const [resizeStart, setResizeStart] = useState({ x: 0, y: 0, width: 0, height: 0 });
  const [isAnimating, setIsAnimating] = useState(false);
  const windowRef = useRef(null);

  const handleMouseDown = (e) => {
    // Don't start dragging if clicking on window controls
    if (e.target.classList.contains('window-btn') || 
        e.target.closest('.window-controls')) {
      return;
    }
    
    if (e.target.classList.contains('window-title-bar') || 
        e.target.closest('.window-title-bar')) {
      setIsDragging(true);
      const rect = windowRef.current.getBoundingClientRect();
      setDragOffset({
        x: e.clientX - rect.left,
        y: e.clientY - rect.top
      });
      onFocus && onFocus(id);
    }
  };

  const handleResizeMouseDown = (e, direction) => {
    e.stopPropagation();
    setIsResizing(true);
    setResizeDirection(direction);
    setResizeStart({
      x: e.clientX,
      y: e.clientY,
      width: size.width,
      height: size.height
    });
    onFocus && onFocus(id);
  };

  const handleMouseMove = (e) => {
    if (isDragging && !isMaximized) {
      setPosition({
        x: e.clientX - dragOffset.x,
        y: Math.max(0, e.clientY - dragOffset.y)
      });
    } else if (isResizing && !isMaximized) {
      const deltaX = e.clientX - resizeStart.x;
      const deltaY = e.clientY - resizeStart.y;
      
      let newWidth = resizeStart.width;
      let newHeight = resizeStart.height;
      let newX = position.x;
      let newY = position.y;
      
      // Handle different resize directions
      if (resizeDirection.includes('right')) {
        newWidth = Math.max(300, resizeStart.width + deltaX);
      }
      if (resizeDirection.includes('left')) {
        newWidth = Math.max(300, resizeStart.width - deltaX);
        newX = position.x + (resizeStart.width - newWidth);
      }
      if (resizeDirection.includes('bottom')) {
        newHeight = Math.max(200, resizeStart.height + deltaY);
      }
      if (resizeDirection.includes('top')) {
        newHeight = Math.max(200, resizeStart.height - deltaY);
        newY = position.y + (resizeStart.height - newHeight);
      }
      
      setSize({ width: newWidth, height: newHeight });
      if (newX !== position.x || newY !== position.y) {
        setPosition({ x: newX, y: newY });
      }
    }
  };

  const handleMouseUp = () => {
    setIsDragging(false);
    setIsResizing(false);
    setResizeDirection('');
  };

  const handleMinimize = (e) => {
    e.stopPropagation();
    e.preventDefault();
    setIsAnimating(true);
    setTimeout(() => {
      onMinimize(id);
      setIsAnimating(false);
    }, 200);
  };

  const handleMaximize = (e) => {
    e.stopPropagation();
    e.preventDefault();
    setIsAnimating(true);
    setTimeout(() => {
      onMaximize(id);
      setIsAnimating(false);
    }, 200);
  };

  const handleWindowClick = (e) => {
    // Don't focus if clicking on window controls
    if (e.target.classList.contains('window-btn') || 
        e.target.closest('.window-controls')) {
      return;
    }
    onFocus && onFocus(id);
  };

  const handleClose = (e) => {
    e.stopPropagation();
    e.preventDefault();
    onClose(id);
  };

  useEffect(() => {
    if (isDragging || isResizing) {
      document.addEventListener('mousemove', handleMouseMove);
      document.addEventListener('mouseup', handleMouseUp);
      return () => {
        document.removeEventListener('mousemove', handleMouseMove);
        document.removeEventListener('mouseup', handleMouseUp);
      };
    }
  }, [isDragging, isResizing, dragOffset, resizeStart, position, size]);

  const windowStyle = isMaximized 
    ? { top: 0, left: 0, width: '100%', height: 'calc(100vh - 40px)', zIndex }
    : { 
        top: position.y, 
        left: position.x, 
        width: size.width, 
        height: size.height, 
        zIndex 
      };

  return (
    <div 
      ref={windowRef}
      className={`window ${isAnimating ? 'window-animating' : ''}`}
      style={windowStyle}
      onMouseDown={handleMouseDown}
      onClick={handleWindowClick}
    >
      <div className="window-title-bar">
        <span className="window-title">{title}</span>
        <div className="window-controls">
          <button className="window-btn minimize-btn" onClick={handleMinimize}>_</button>
          <button className="window-btn maximize-btn" onClick={handleMaximize}>□</button>
          <button className="window-btn close-btn" onClick={handleClose}>×</button>
        </div>
      </div>
      <div className="window-content">
        {children}
      </div>
      
      {/* Resize handles */}
      {!isMaximized && (
        <>
          {/* Corner handles */}
          <div className="resize-handle resize-nw" onMouseDown={(e) => handleResizeMouseDown(e, 'top-left')} />
          <div className="resize-handle resize-ne" onMouseDown={(e) => handleResizeMouseDown(e, 'top-right')} />
          <div className="resize-handle resize-sw" onMouseDown={(e) => handleResizeMouseDown(e, 'bottom-left')} />
          <div className="resize-handle resize-se" onMouseDown={(e) => handleResizeMouseDown(e, 'bottom-right')} />
          
          {/* Edge handles */}
          <div className="resize-handle resize-n" onMouseDown={(e) => handleResizeMouseDown(e, 'top')} />
          <div className="resize-handle resize-s" onMouseDown={(e) => handleResizeMouseDown(e, 'bottom')} />
          <div className="resize-handle resize-w" onMouseDown={(e) => handleResizeMouseDown(e, 'left')} />
          <div className="resize-handle resize-e" onMouseDown={(e) => handleResizeMouseDown(e, 'right')} />
        </>
      )}
    </div>
  );
};

// Enhanced Start Menu Component  
const StartMenu = ({ isOpen, onClose, onOpenApp, onShutDown, user }) => {
  const [submenuOpen, setSubmenuOpen] = useState(null);

  if (!isOpen) return null;

  const menuItems = [
    {
      label: 'Programs',
      icon: '📁',
      submenu: [
        { label: 'Calculator', icon: '🔢', action: () => onOpenApp('calculator', 'Calculator') },
        { label: 'Notepad', icon: '📝', action: () => onOpenApp('texteditor', 'Notepad') },
        { label: 'Paint', icon: '🎨', action: () => onOpenApp('paint', 'Paint') },
        { label: 'File Browser', icon: '📂', action: () => onOpenApp('filebrowser', 'File Browser') },
        { label: 'Web Browser', icon: '🌐', action: () => onOpenApp('webbrowser', 'Web Browser') },
        { label: 'Games', icon: '🎮', action: () => onOpenApp('games', 'Games') },
        { label: 'Task Manager', icon: '📊', action: () => onOpenApp('taskmanager', 'Task Manager') }
      ]
    },
    {
      label: 'Documents',
      icon: '📄',
      action: () => onOpenApp('filebrowser', 'Documents')
    },
    {
      label: 'Settings',
      icon: '⚙️',
      submenu: [
        { label: 'Control Panel', icon: '🎛️', action: () => onOpenApp('controlpanel', 'Control Panel') },
        { label: 'System Information', icon: 'ℹ️', action: () => onOpenApp('systeminfo', 'System Information') },
        { label: 'Task Manager', icon: '📊', action: () => onOpenApp('taskmanager', 'Task Manager') }
      ]
    },
    {
      label: 'Find',
      icon: '🔍',
      submenu: [
        { label: 'Files or Folders...', action: () => onOpenApp('search', 'Search') },
        { label: 'Computer', action: () => onOpenApp('filebrowser', 'Computer') }
      ]
    },
    { label: 'Help', icon: '❓', action: () => onOpenApp('help', 'Help') },
    { label: 'Run...', icon: '🏃', action: () => onOpenApp('run', 'Run') },
    { label: 'Log Off...', icon: '👤', action: () => onShutDown() },
    { label: 'Shut Down...', icon: '🔴', action: () => onShutDown() }
  ];

  return (
    <div className="start-menu-overlay" onClick={onClose}>
      <div className="start-menu" onClick={(e) => e.stopPropagation()}>
        <div className="start-menu-header">
          <div className="start-menu-logo">🏠</div>
          <div className="start-menu-user">
            <div className="user-name">{user?.username}</div>
            <div className="os-name">RetroOS</div>
          </div>
        </div>
        <div className="start-menu-items">
          {menuItems.map((item, index) => (
            <div key={index} className="start-menu-item">
              <div 
                className="start-menu-item-main"
                onMouseEnter={() => setSubmenuOpen(item.submenu ? index : null)}
                onClick={() => {
                  if (!item.submenu && item.action) {
                    item.action();
                    onClose();
                  }
                }}
              >
                <span className="start-menu-icon">{item.icon}</span>
                <span className="start-menu-label">{item.label}</span>
                {item.submenu && <span className="start-menu-arrow">▶</span>}
              </div>
              {item.submenu && submenuOpen === index && (
                <div className="start-submenu">
                  {item.submenu.map((subitem, subindex) => (
                    <div 
                      key={subindex}
                      className="start-submenu-item"
                      onClick={() => {
                        subitem.action();
                        onClose();
                      }}
                    >
                      <span className="start-menu-icon">{subitem.icon}</span>
                      <span className="start-menu-label">{subitem.label}</span>
                    </div>
                  ))}
                </div>
              )}
            </div>
          ))}
        </div>
      </div>
    </div>
  );
};

// Enhanced Context Menu Component
const ContextMenu = ({ isOpen, position, onClose, onAction }) => {
  if (!isOpen) return null;

  const menuItems = [
    { 
      label: 'New', 
      icon: '📄', 
      submenu: [
        { label: 'Folder', action: () => onAction('new-folder') },
        { label: 'Text Document', action: () => onAction('new-text') },
        { label: 'Bitmap Image', action: () => onAction('new-image') }
      ]
    },
    { label: 'Arrange Icons', icon: '📐', submenu: [
      { label: 'By Name', action: () => onAction('arrange-name') },
      { label: 'By Type', action: () => onAction('arrange-type') },
      { label: 'Auto Arrange', action: () => onAction('arrange-auto') }
    ]},
    { label: 'View', icon: '👁️', submenu: [
      { label: 'Large Icons', action: () => onAction('view-large') },
      { label: 'Small Icons', action: () => onAction('view-small') },
      { label: 'Details', action: () => onAction('view-details') }
    ]},
    { label: 'Refresh', icon: '🔄', action: () => onAction('refresh') },
    { label: 'Paste', icon: '📋', action: () => onAction('paste') },
    { label: 'Properties', icon: '📋', action: () => onAction('properties') }
  ];

  return (
    <div className="context-menu-overlay" onClick={onClose}>
      <div 
        className="context-menu" 
        style={{ top: position.y, left: position.x }}
        onClick={(e) => e.stopPropagation()}
      >
        {menuItems.map((item, index) => (
          <div key={index} className="context-menu-item">
            <div 
              className="context-menu-item-main"
              onClick={() => {
                if (!item.submenu && item.action) {
                  item.action();
                  onClose();
                }
              }}
            >
              <span className="context-menu-icon">{item.icon}</span>
              <span className="context-menu-label">{item.label}</span>
              {item.submenu && <span className="context-menu-arrow">▶</span>}
            </div>
            {item.submenu && (
              <div className="context-submenu">
                {item.submenu.map((subitem, subindex) => (
                  <div 
                    key={subindex}
                    className="context-submenu-item"
                    onClick={() => {
                      subitem.action();
                      onClose();
                    }}
                  >
                    <span className="context-menu-label">{subitem.label}</span>
                  </div>
                ))}
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
};

// Paint Program Component
const Paint = () => {
  const canvasRef = useRef(null);
  const [isDrawing, setIsDrawing] = useState(false);
  const [tool, setTool] = useState('brush');
  const [color, setColor] = useState('#000000');
  const [brushSize, setBrushSize] = useState(3);

  useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = 'white';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  }, []);

  const startDrawing = (e) => {
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    setIsDrawing(true);
    const ctx = canvas.getContext('2d');
    ctx.beginPath();
    ctx.moveTo(x, y);
  };

  const draw = (e) => {
    if (!isDrawing) return;
    
    const canvas = canvasRef.current;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    const ctx = canvas.getContext('2d');
    ctx.strokeStyle = color;
    ctx.lineWidth = brushSize;
    ctx.lineCap = 'round';
    ctx.lineTo(x, y);
    ctx.stroke();
  };

  const stopDrawing = () => {
    setIsDrawing(false);
  };

  const clearCanvas = () => {
    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = 'white';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  };

  const colors = ['#000000', '#FF0000', '#00FF00', '#0000FF', '#FFFF00', '#FF00FF', '#00FFFF', '#FFA500', '#800080', '#FFC0CB'];

  return (
    <div className="paint-app">
      <div className="paint-toolbar">
        <div className="paint-tools">
          <button className={`paint-tool ${tool === 'brush' ? 'active' : ''}`} onClick={() => setTool('brush')}>🖌️</button>
          <button className={`paint-tool ${tool === 'eraser' ? 'active' : ''}`} onClick={() => setTool('eraser')}>🧹</button>
          <button className="paint-tool" onClick={clearCanvas}>🗑️</button>
        </div>
        <div className="paint-colors">
          {colors.map(c => (
            <div 
              key={c}
              className={`color-swatch ${color === c ? 'active' : ''}`}
              style={{ backgroundColor: c }}
              onClick={() => setColor(c)}
            />
          ))}
        </div>
        <div className="paint-brush-size">
          <label>Size: </label>
          <input 
            type="range" 
            min="1" 
            max="20" 
            value={brushSize} 
            onChange={(e) => setBrushSize(e.target.value)}
          />
          <span>{brushSize}px</span>
        </div>
      </div>
      <canvas
        ref={canvasRef}
        width={800}
        height={600}
        className="paint-canvas"
        onMouseDown={startDrawing}
        onMouseMove={draw}
        onMouseUp={stopDrawing}
        onMouseLeave={stopDrawing}
      />
    </div>
  );
};

// Control Panel Component
const ControlPanel = ({ user, onUpdateSettings }) => {
  const [wallpapers, setWallpapers] = useState([]);
  const [selectedWallpaper, setSelectedWallpaper] = useState(user?.desktop_settings?.wallpaper || 'classic_clouds');
  const [activeTab, setActiveTab] = useState('display');
  const [settings, setSettings] = useState({
    taskbarPosition: user?.desktop_settings?.taskbar_settings?.position || 'bottom',
    doubleClickSpeed: user?.desktop_settings?.doubleClickSpeed || 'normal',
    windowAnimations: user?.desktop_settings?.windowAnimations !== false,
    soundEnabled: user?.desktop_settings?.soundEnabled !== false,
    iconSize: user?.desktop_settings?.iconSize || 'normal',
    theme: user?.desktop_settings?.theme || 'classic'
  });

  useEffect(() => {
    loadWallpapers();
  }, []);

  const loadWallpapers = async () => {
    try {
      const response = await axios.get(`${API}/wallpapers`);
      setWallpapers(response.data);
    } catch (error) {
      console.error('Error loading wallpapers:', error);
    }
  };

  const applyWallpaper = async () => {
    try {
      const newSettings = {
        ...user.desktop_settings,
        wallpaper: selectedWallpaper
      };
      
      await axios.put(`${API}/user/${user.user_id}/desktop-settings`, newSettings);
      onUpdateSettings(newSettings);
      alert('Wallpaper changed successfully!');
    } catch (error) {
      console.error('Error updating wallpaper:', error);
    }
  };

  const applySettings = async () => {
    try {
      const newSettings = {
        ...user.desktop_settings,
        ...settings,
        taskbar_settings: { position: settings.taskbarPosition }
      };
      
      await axios.put(`${API}/user/${user.user_id}/desktop-settings`, newSettings);
      onUpdateSettings(newSettings);
      alert('Settings applied successfully!');
    } catch (error) {
      console.error('Error updating settings:', error);
    }
  };

  const handleSettingChange = (setting, value) => {
    setSettings(prev => ({ ...prev, [setting]: value }));
  };

  const tabs = [
    { id: 'display', label: 'Display', icon: '🖥️' },
    { id: 'interface', label: 'Interface', icon: '🎛️' },
    { id: 'system', label: 'System', icon: '⚙️' },
    { id: 'accessibility', label: 'Access', icon: '♿' }
  ];

  return (
    <div className="control-panel">
      <div className="control-panel-tabs">
        {tabs.map(tab => (
          <div 
            key={tab.id}
            className={`control-panel-tab ${activeTab === tab.id ? 'active' : ''}`}
            onClick={() => setActiveTab(tab.id)}
          >
            <span className="tab-icon">{tab.icon}</span>
            <span className="tab-label">{tab.label}</span>
          </div>
        ))}
      </div>
      
      <div className="control-panel-content">
        {activeTab === 'display' && (
          <div className="settings-section">
            <h3>Desktop Wallpaper</h3>
            <div className="wallpaper-grid">
              {wallpapers.map(wallpaper => (
                <div 
                  key={wallpaper.id}
                  className={`wallpaper-option ${selectedWallpaper === wallpaper.id ? 'selected' : ''}`}
                  onClick={() => setSelectedWallpaper(wallpaper.id)}
                >
                  <div className={`wallpaper-preview wallpaper-${wallpaper.id}`}></div>
                  <div className="wallpaper-name">{wallpaper.name}</div>
                </div>
              ))}
            </div>
            <button onClick={applyWallpaper} className="apply-btn">Apply Wallpaper</button>
            
            <h3>Screen Resolution</h3>
            <div className="setting-group">
              <label>Resolution:</label>
              <select value="1024x768" onChange={() => {}}>
                <option value="800x600">800 x 600</option>
                <option value="1024x768">1024 x 768</option>
                <option value="1280x1024">1280 x 1024</option>
                <option value="1920x1080">1920 x 1080</option>
              </select>
            </div>
          </div>
        )}
        
        {activeTab === 'interface' && (
          <div className="settings-section">
            <h3>Taskbar Settings</h3>
            <div className="setting-group">
              <label>Position:</label>
              <select 
                value={settings.taskbarPosition} 
                onChange={(e) => handleSettingChange('taskbarPosition', e.target.value)}
              >
                <option value="bottom">Bottom</option>
                <option value="top">Top</option>
                <option value="left">Left</option>
                <option value="right">Right</option>
              </select>
            </div>
            
            <h3>Desktop Icons</h3>
            <div className="setting-group">
              <label>Icon Size:</label>
              <select 
                value={settings.iconSize} 
                onChange={(e) => handleSettingChange('iconSize', e.target.value)}
              >
                <option value="small">Small</option>
                <option value="normal">Normal</option>
                <option value="large">Large</option>
              </select>
            </div>
            
            <h3>Theme</h3>
            <div className="setting-group">
              <label>Color Scheme:</label>
              <select 
                value={settings.theme} 
                onChange={(e) => handleSettingChange('theme', e.target.value)}
              >
                <option value="classic">Windows Classic</option>
                <option value="blue">Windows Blue</option>
                <option value="green">Windows Green</option>
                <option value="high-contrast">High Contrast</option>
              </select>
            </div>
          </div>
        )}
        
        {activeTab === 'system' && (
          <div className="settings-section">
            <h3>Performance</h3>
            <div className="setting-group">
              <label>
                <input 
                  type="checkbox" 
                  checked={settings.windowAnimations}
                  onChange={(e) => handleSettingChange('windowAnimations', e.target.checked)}
                />
                Enable window animations
              </label>
            </div>
            
            <h3>Sound</h3>
            <div className="setting-group">
              <label>
                <input 
                  type="checkbox" 
                  checked={settings.soundEnabled}
                  onChange={(e) => handleSettingChange('soundEnabled', e.target.checked)}
                />
                Enable system sounds
              </label>
            </div>
            
            <h3>Mouse</h3>
            <div className="setting-group">
              <label>Double-click speed:</label>
              <select 
                value={settings.doubleClickSpeed} 
                onChange={(e) => handleSettingChange('doubleClickSpeed', e.target.value)}
              >
                <option value="slow">Slow</option>
                <option value="normal">Normal</option>
                <option value="fast">Fast</option>
              </select>
            </div>
          </div>
        )}
        
        {activeTab === 'accessibility' && (
          <div className="settings-section">
            <h3>Visual</h3>
            <div className="setting-group">
              <label>
                <input type="checkbox" />
                Use high contrast
              </label>
            </div>
            <div className="setting-group">
              <label>
                <input type="checkbox" />
                Show large icons
              </label>
            </div>
            
            <h3>Keyboard</h3>
            <div className="setting-group">
              <label>
                <input type="checkbox" />
                Sticky keys
              </label>
            </div>
            <div className="setting-group">
              <label>
                <input type="checkbox" />
                Filter keys
              </label>
            </div>
            
            <h3>Mouse</h3>
            <div className="setting-group">
              <label>
                <input type="checkbox" />
                Mouse keys
              </label>
            </div>
          </div>
        )}
        
        <div className="control-panel-footer">
          <button onClick={applySettings} className="apply-btn">Apply All Settings</button>
          <button onClick={() => window.location.reload()} className="apply-btn">Reset to Defaults</button>
        </div>
      </div>
    </div>
  );
};

// System Information Component
const SystemInfo = () => {
  const [systemInfo, setSystemInfo] = useState(null);

  useEffect(() => {
    loadSystemInfo();
  }, []);

  const loadSystemInfo = async () => {
    try {
      const response = await axios.get(`${API}/system/info`);
      setSystemInfo(response.data);
    } catch (error) {
      console.error('Error loading system info:', error);
    }
  };

  return (
    <div className="system-info">
      <div className="system-info-header">
        <div className="system-logo">🖥️</div>
        <div className="system-title">
          <h2>{systemInfo?.os_name}</h2>
          <p>Version {systemInfo?.version}</p>
        </div>
      </div>
      <div className="system-details">
        <div className="detail-row">
          <span className="detail-label">Build:</span>
          <span className="detail-value">{systemInfo?.build}</span>
        </div>
        <div className="detail-row">
          <span className="detail-label">Status:</span>
          <span className="detail-value">{systemInfo?.uptime}</span>
        </div>
        <div className="detail-row">
          <span className="detail-label">Features:</span>
          <div className="feature-list">
            {systemInfo?.features?.map((feature, index) => (
              <span key={index} className="feature-item">• {feature}</span>
            ))}
          </div>
        </div>
      </div>
      <div className="system-info-footer">
        <p>© 2024 RetroOS. All rights reserved.</p>
      </div>
    </div>
  );
};

// Run Dialog Component
const RunDialog = () => {
  const [command, setCommand] = useState('');

  const handleSubmit = (e) => {
    e.preventDefault();
    alert(`Command "${command}" executed!`);
    setCommand('');
  };

  return (
    <div className="run-dialog">
      <h3>Run</h3>
      <p>Type the name of a program, folder, document, or Internet resource, and RetroOS will open it for you.</p>
      <form onSubmit={handleSubmit}>
        <div className="run-input-group">
          <label>Open:</label>
          <input 
            type="text" 
            value={command}
            onChange={(e) => setCommand(e.target.value)}
            className="run-input"
            placeholder="Enter command..."
          />
        </div>
        <div className="run-buttons">
          <button type="submit">OK</button>
          <button type="button">Cancel</button>
          <button type="button">Browse...</button>
        </div>
      </form>
    </div>
  );
};

// Recycle Bin Component
const RecycleBin = ({ user }) => {
  const [deletedFiles, setDeletedFiles] = useState([]);

  useEffect(() => {
    if (user) {
      loadDeletedFiles();
    }
  }, [user]);

  const loadDeletedFiles = async () => {
    try {
      const response = await axios.get(`${API}/files/${user.user_id}/recycle-bin`);
      setDeletedFiles(response.data);
    } catch (error) {
      console.error('Error loading deleted files:', error);
    }
  };

  const restoreFile = async (fileId) => {
    try {
      await axios.post(`${API}/files/${user.user_id}/${fileId}/restore`);
      loadDeletedFiles();
      alert('File restored successfully!');
    } catch (error) {
      console.error('Error restoring file:', error);
    }
  };

  const deleteFilePermanently = async (fileId) => {
    if (window.confirm('Are you sure you want to permanently delete this file?')) {
      try {
        await axios.delete(`${API}/files/${user.user_id}/${fileId}?permanent=true`);
        loadDeletedFiles();
      } catch (error) {
        console.error('Error deleting file permanently:', error);
      }
    }
  };

  const emptyRecycleBin = async () => {
    if (window.confirm('Are you sure you want to permanently delete all items in the Recycle Bin?')) {
      try {
        await axios.post(`${API}/files/${user.user_id}/empty-recycle-bin`);
        loadDeletedFiles();
        alert('Recycle Bin emptied successfully!');
      } catch (error) {
        console.error('Error emptying recycle bin:', error);
      }
    }
  };

  return (
    <div className="recycle-bin">
      <div className="recycle-bin-toolbar">
        <button onClick={emptyRecycleBin} className="empty-bin-btn" disabled={deletedFiles.length === 0}>
          Empty Recycle Bin
        </button>
      </div>
      <div className="deleted-files-list">
        {deletedFiles.length === 0 ? (
          <div className="empty-message">Recycle Bin is empty</div>
        ) : (
          deletedFiles.map(file => (
            <div key={file.id} className="deleted-file-item">
              <div className="file-icon">
                {file.type === 'folder' ? '📁' : getFileIcon(file.file_extension)}
              </div>
              <div className="file-info">
                <div className="file-name">{file.name}</div>
                <div className="file-details">
                  Deleted: {new Date(file.modified_at).toLocaleDateString()}
                </div>
              </div>
              <div className="file-actions">
                <button onClick={() => restoreFile(file.id)} className="restore-btn">Restore</button>
                <button onClick={() => deleteFilePermanently(file.id)} className="delete-permanent-btn">Delete</button>
              </div>
            </div>
          ))
        )}
      </div>
    </div>
  );
};

// Helper function to get file icons
const getFileIcon = (extension) => {
  const iconMap = {
    'txt': '📄',
    'doc': '📄',
    'pdf': '📕',
    'jpg': '🖼️',
    'jpeg': '🖼️',
    'png': '🖼️',
    'gif': '🖼️',
    'mp3': '🎵',
    'wav': '🎵',
    'mp4': '🎬',
    'avi': '🎬',
    'zip': '📦',
    'exe': '⚙️',
    'html': '🌐',
    'css': '🎨',
    'js': '📜'
  };
  return iconMap[extension] || '📄';
};

// Calculator App (keeping original)
const Calculator = () => {
  const [display, setDisplay] = useState('0');
  const [operation, setOperation] = useState(null);
  const [prevValue, setPrevValue] = useState(null);
  const [waitingForNewValue, setWaitingForNewValue] = useState(false);

  const inputNumber = (num) => {
    if (waitingForNewValue) {
      setDisplay(String(num));
      setWaitingForNewValue(false);
    } else {
      setDisplay(display === '0' ? String(num) : display + num);
    }
  };

  const inputOperation = (nextOperation) => {
    const inputValue = parseFloat(display);

    if (prevValue === null) {
      setPrevValue(inputValue);
    } else if (operation) {
      const currentValue = prevValue || 0;
      const newValue = calculate(currentValue, inputValue, operation);
      
      setDisplay(String(newValue));
      setPrevValue(newValue);
    }

    setWaitingForNewValue(true);
    setOperation(nextOperation);
  };

  const calculate = (firstValue, secondValue, operation) => {
    switch (operation) {
      case '+': return firstValue + secondValue;
      case '-': return firstValue - secondValue;
      case '*': return firstValue * secondValue;
      case '/': return firstValue / secondValue;
      case '=': return secondValue;
      default: return secondValue;
    }
  };

  const performCalculation = () => {
    const inputValue = parseFloat(display);

    if (prevValue !== null && operation) {
      const newValue = calculate(prevValue, inputValue, operation);
      setDisplay(String(newValue));
      setPrevValue(null);
      setOperation(null);
      setWaitingForNewValue(true);
    }
  };

  const allClear = () => {
    setDisplay('0');
    setOperation(null);
    setPrevValue(null);
    setWaitingForNewValue(false);
  };

  return (
    <div className="calculator">
      <div className="calc-display">{display}</div>
      <div className="calc-buttons">
        <button onClick={allClear} className="calc-btn calc-btn-wide">AC</button>
        <button onClick={() => inputOperation('/')} className="calc-btn calc-btn-operator">÷</button>
        
        <button onClick={() => inputNumber(7)} className="calc-btn">7</button>
        <button onClick={() => inputNumber(8)} className="calc-btn">8</button>
        <button onClick={() => inputNumber(9)} className="calc-btn">9</button>
        <button onClick={() => inputOperation('*')} className="calc-btn calc-btn-operator">×</button>
        
        <button onClick={() => inputNumber(4)} className="calc-btn">4</button>
        <button onClick={() => inputNumber(5)} className="calc-btn">5</button>
        <button onClick={() => inputNumber(6)} className="calc-btn">6</button>
        <button onClick={() => inputOperation('-')} className="calc-btn calc-btn-operator">-</button>
        
        <button onClick={() => inputNumber(1)} className="calc-btn">1</button>
        <button onClick={() => inputNumber(2)} className="calc-btn">2</button>
        <button onClick={() => inputNumber(3)} className="calc-btn">3</button>
        <button onClick={() => inputOperation('+')} className="calc-btn calc-btn-operator">+</button>
        
        <button onClick={() => inputNumber(0)} className="calc-btn calc-btn-wide">0</button>
        <button onClick={performCalculation} className="calc-btn calc-btn-operator">=</button>
      </div>
    </div>
  );
};

// Enhanced Text Editor App
const TextEditor = ({ user, onSave, initialFile = null }) => {
  const [content, setContent] = useState('');
  const [fileName, setFileName] = useState('Untitled.txt');
  const [currentFile, setCurrentFile] = useState(null);
  const [isSaved, setIsSaved] = useState(true);
  const [isModified, setIsModified] = useState(false);
  const [showSaveAsDialog, setShowSaveAsDialog] = useState(false);
  const [saveDialogFolders, setSaveDialogFolders] = useState([]);
  const [saveDialogCurrentPath, setSaveDialogCurrentPath] = useState('/');
  const [saveDialogCurrentFolderId, setSaveDialogCurrentFolderId] = useState(null);
  const [saveAsName, setSaveAsName] = useState('');
  const [saveDialogLoading, setSaveDialogLoading] = useState(false);

  useEffect(() => {
    if (initialFile) {
      loadFile(initialFile);
    }
  }, [initialFile]);

  const loadFile = async (file) => {
    try {
      const response = await axios.get(`${API}/files/${user.user_id}/${file.id}`);
      setContent(response.data.content || '');
      setFileName(response.data.name);
      setCurrentFile(response.data);
      setIsSaved(true);
      setIsModified(false);
    } catch (error) {
      console.error('Error loading file:', error);
      alert('Error loading file');
    }
  };

  const handleContentChange = (e) => {
    setContent(e.target.value);
    setIsSaved(false);
    setIsModified(true);
  };

  const saveFile = async (saveAs = false) => {
    try {
      if (currentFile && !saveAs) {
        // Update existing file
        await axios.put(`${API}/files/${user.user_id}/${currentFile.id}`, {
          content: content
        });
        setIsSaved(true);
        setIsModified(false);
        onSave && onSave();
      } else {
        // Save as new file or Save As
        await loadSaveDialogFolders('/');
        setShowSaveAsDialog(true);
        setSaveAsName(currentFile ? currentFile.name : 'Untitled.txt');
      }
    } catch (error) {
      console.error('Error saving file:', error);
      alert(`Error saving file: ${error.response?.data?.detail || error.message}`);
    }
  };

  const loadSaveDialogFolders = async (path = '/', folderId = null) => {
    setSaveDialogLoading(true);
    try {
      let url = `${API}/files/${user.user_id}`;
      if (folderId) {
        url += `?parent_id=${folderId}`;
      } else if (path !== '/') {
        url += `?path=${encodeURIComponent(path)}`;
      }
      
      const response = await axios.get(url);
      // Only show folders in save dialog
      const folders = response.data.filter(item => item.type === 'folder');
      setSaveDialogFolders(folders);
      setSaveDialogCurrentPath(path);
      setSaveDialogCurrentFolderId(folderId);
    } catch (error) {
      console.error('Error loading folders:', error);
    }
    setSaveDialogLoading(false);
  };

  const navigateToSaveFolder = async (folder) => {
    const newPath = saveDialogCurrentPath === '/' ? `/${folder.name}` : `${saveDialogCurrentPath}/${folder.name}`;
    await loadSaveDialogFolders(newPath, folder.id);
  };

  const navigateSaveDialogUp = async () => {
    if (saveDialogCurrentPath === '/') return;
    
    const pathParts = saveDialogCurrentPath.split('/').filter(p => p);
    if (pathParts.length === 1) {
      // Go to root
      await loadSaveDialogFolders('/');
    } else {
      // Go to parent
      const parentPath = '/' + pathParts.slice(0, -1).join('/');
      await loadSaveDialogFolders(parentPath);
    }
  };

  const saveAsNewFile = async () => {
    if (!saveAsName.trim()) {
      alert('Please enter a file name');
      return;
    }

    // Ensure file has extension
    let finalFileName = saveAsName.trim();
    if (!finalFileName.includes('.')) {
      finalFileName += '.txt';
    }

    try {
      const newPath = saveDialogCurrentPath === '/' ? `/${finalFileName}` : `${saveDialogCurrentPath}/${finalFileName}`;
      
      console.log('Saving file with data:', {
        name: finalFileName,
        type: 'file',
        content: content,
        parent_id: saveDialogCurrentFolderId,
        path: newPath
      });

      const response = await axios.post(`${API}/files/${user.user_id}`, {
        name: finalFileName,
        type: 'file',
        content: content,
        parent_id: saveDialogCurrentFolderId,
        path: newPath
      });
      
      setCurrentFile(response.data);
      setFileName(finalFileName);
      setIsSaved(true);
      setIsModified(false);
      setShowSaveAsDialog(false);
      setSaveAsName('');
      setSaveDialogCurrentPath('/');
      setSaveDialogCurrentFolderId(null);
      onSave && onSave();
      
      alert('File saved successfully!');
    } catch (error) {
      console.error('Error saving file:', error);
      let errorMessage = 'Error saving file';
      if (error.response?.data?.detail) {
        if (error.response.data.detail === 'File already exists') {
          errorMessage = `A file named "${finalFileName}" already exists in this location.`;
        } else {
          errorMessage = error.response.data.detail;
        }
      }
      alert(errorMessage);
    }
  };

  const newFile = () => {
    if (isModified) {
      if (window.confirm('You have unsaved changes. Are you sure you want to create a new file?')) {
        setContent('');
        setFileName('Untitled.txt');
        setCurrentFile(null);
        setIsSaved(true);
        setIsModified(false);
      }
    } else {
      setContent('');
      setFileName('Untitled.txt');
      setCurrentFile(null);
      setIsSaved(true);
      setIsModified(false);
    }
  };

  const openFile = () => {
    alert('To open a file, use the File Browser and double-click on a text file.');
  };

  const getSaveDialogPathSegments = () => {
    if (saveDialogCurrentPath === '/') return [{ name: 'Root', path: '/' }];
    const parts = saveDialogCurrentPath.split('/').filter(p => p);
    const segments = [{ name: 'Root', path: '/' }];
    let buildPath = '';
    parts.forEach(part => {
      buildPath += `/${part}`;
      segments.push({ name: part, path: buildPath });
    });
    return segments;
  };

  return (
    <div className="text-editor">
      <div className="text-editor-menubar">
        <div className="menu-group">
          <button onClick={newFile} className="menu-btn" title="New File">📄 New</button>
          <button onClick={openFile} className="menu-btn" title="Open File">📂 Open</button>
          <button onClick={() => saveFile(false)} className="menu-btn" disabled={isSaved} title="Save">
            💾 Save {isModified ? '*' : ''}
          </button>
          <button onClick={() => saveFile(true)} className="menu-btn" title="Save As">
            💾 Save As...
          </button>
        </div>
      </div>
      
      <div className="text-editor-toolbar">
        <div className="file-info">
          <span className="current-file">{currentFile ? currentFile.name : fileName}{isModified ? ' *' : ''}</span>
          <span className="file-path">{currentFile ? currentFile.path : 'Not saved'}</span>
        </div>
        <div className="editor-stats">
          <span>Lines: {content.split('\n').length}</span>
          <span>Characters: {content.length}</span>
          <span>Words: {content.trim().split(/\s+/).filter(w => w).length}</span>
        </div>
      </div>
      
      <textarea
        value={content}
        onChange={handleContentChange}
        className="text-editor-content"
        placeholder="Start typing..."
        spellCheck={false}
      />

      {/* Enhanced Save As Dialog */}
      {showSaveAsDialog && (
        <div className="modal-overlay">
          <div className="modal-dialog save-as-dialog-enhanced">
            <div className="save-dialog-header">
              <h3>Save As</h3>
              <button 
                className="close-btn"
                onClick={() => {
                  setShowSaveAsDialog(false);
                  setSaveAsName('');
                  setSaveDialogCurrentPath('/');
                  setSaveDialogCurrentFolderId(null);
                }}
              >
                ×
              </button>
            </div>
            
            <div className="save-dialog-content">
              {/* Navigation */}
              <div className="save-dialog-navigation">
                <button 
                  onClick={navigateSaveDialogUp}
                  disabled={saveDialogCurrentPath === '/'}
                  className="nav-btn"
                  title="Up"
                >
                  ⬆️
                </button>
                <div className="save-dialog-path">
                  {getSaveDialogPathSegments().map((segment, index) => (
                    <span key={index}>
                      <button 
                        className="path-segment"
                        onClick={() => loadSaveDialogFolders(segment.path)}
                      >
                        {segment.name}
                      </button>
                      {index < getSaveDialogPathSegments().length - 1 && <span className="path-separator"> › </span>}
                    </span>
                  ))}
                </div>
              </div>

              {/* Folder List */}
              <div className="save-dialog-folders">
                <div className="folders-header">Choose a folder:</div>
                <div className="folders-list">
                  {saveDialogLoading ? (
                    <div className="loading">Loading folders...</div>
                  ) : saveDialogFolders.length === 0 ? (
                    <div className="no-folders">No folders in this location</div>
                  ) : (
                    saveDialogFolders.map(folder => (
                      <div 
                        key={folder.id}
                        className="folder-item"
                        onDoubleClick={() => navigateToSaveFolder(folder)}
                      >
                        <span className="folder-icon">📁</span>
                        <span className="folder-name">{folder.name}</span>
                      </div>
                    ))
                  )}
                </div>
              </div>

              {/* File Name Input */}
              <div className="save-dialog-filename">
                <label>File name:</label>
                <input 
                  type="text" 
                  value={saveAsName}
                  onChange={(e) => setSaveAsName(e.target.value)}
                  placeholder="Enter file name..."
                  onKeyPress={(e) => e.key === 'Enter' && saveAsNewFile()}
                  className="filename-input"
                />
                <div className="file-type-hint">
                  {!saveAsName.includes('.') && "Will be saved as .txt file"}
                </div>
              </div>
            </div>
            
            <div className="save-dialog-footer">
              <div className="current-location">Save in: {saveDialogCurrentPath}</div>
              <div className="save-dialog-buttons">
                <button onClick={saveAsNewFile} disabled={!saveAsName.trim()}>Save</button>
                <button onClick={() => {
                  setShowSaveAsDialog(false);
                  setSaveAsName('');
                  setSaveDialogCurrentPath('/');
                  setSaveDialogCurrentFolderId(null);
                }}>Cancel</button>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

// Enhanced File Browser App
const FileBrowser = ({ user, onRefresh }) => {
  const [files, setFiles] = useState([]);
  const [currentPath, setCurrentPath] = useState('/');
  const [currentFolderId, setCurrentFolderId] = useState(null);
  const [loading, setLoading] = useState(false);
  const [viewMode, setViewMode] = useState('list'); // 'list' or 'icons'
  const [selectedFiles, setSelectedFiles] = useState([]);
  const [pathHistory, setPathHistory] = useState(['/']);
  const [showNewFolderDialog, setShowNewFolderDialog] = useState(false);
  const [newFolderName, setNewFolderName] = useState('');

  const loadFiles = async (folderId = null, path = '/') => {
    setLoading(true);
    try {
      let url = `${API}/files/${user.user_id}`;
      if (folderId) {
        url += `?parent_id=${folderId}`;
      } else if (path === '/') {
        // For root directory, don't include parent_id parameter at all
        // The backend will default to parent_id = None
      } else {
        url += `?path=${encodeURIComponent(path)}`;
      }
      
      console.log('Loading files with URL:', url);
      const response = await axios.get(url);
      console.log('Files loaded:', response.data.length, 'files');
      setFiles(response.data);
      setCurrentPath(path);
      setCurrentFolderId(folderId);
      setSelectedFiles([]);
    } catch (error) {
      console.error('Error loading files:', error);
      alert('Error loading files');
    }
    setLoading(false);
  };

  useEffect(() => {
    if (user) {
      loadFiles();
    }
  }, [user]);

  const navigateToFolder = async (folder) => {
    const newPath = currentPath === '/' ? `/${folder.name}` : `${currentPath}/${folder.name}`;
    setPathHistory([...pathHistory, newPath]);
    await loadFiles(folder.id, newPath);
  };

  const navigateUp = async () => {
    if (pathHistory.length > 1) {
      const newHistory = pathHistory.slice(0, -1);
      const parentPath = newHistory[newHistory.length - 1];
      setPathHistory(newHistory);
      
      // Find parent folder ID
      let parentId = null;
      if (parentPath !== '/') {
        try {
          const response = await axios.get(`${API}/files/${user.user_id}`);
          const allFiles = response.data;
          const parentFolder = allFiles.find(f => f.path === parentPath && f.type === 'folder');
          parentId = parentFolder?.id || null;
        } catch (error) {
          console.error('Error finding parent folder:', error);
        }
      }
      
      await loadFiles(parentId, parentPath);
    }
  };

  const navigateToPath = async (targetPath) => {
    if (targetPath === '/') {
      setPathHistory(['/']);
      await loadFiles(null, '/');
    } else {
      // Build path history
      const pathParts = targetPath.split('/').filter(p => p);
      const history = ['/'];
      let buildPath = '';
      pathParts.forEach(part => {
        buildPath += `/${part}`;
        history.push(buildPath);
      });
      setPathHistory(history);
      
      // Find target folder ID
      try {
        const response = await axios.get(`${API}/files/${user.user_id}`);
        const allFiles = response.data;
        const targetFolder = allFiles.find(f => f.path === targetPath && f.type === 'folder');
        await loadFiles(targetFolder?.id || null, targetPath);
      } catch (error) {
        console.error('Error navigating to path:', error);
        await loadFiles(null, targetPath);
      }
    }
  };

  const createFolder = async () => {
    if (!newFolderName.trim()) return;
    
    // Check if folder already exists in current location
    const existingFolder = files.find(file => 
      file.name.toLowerCase() === newFolderName.toLowerCase() && 
      file.type === 'folder'
    );
    
    if (existingFolder) {
      alert(`A folder named "${newFolderName}" already exists in this location.`);
      return;
    }
    
    try {
      const newPath = currentPath === '/' ? `/${newFolderName}` : `${currentPath}/${newFolderName}`;
      console.log('Creating folder with data:', {
        name: newFolderName,
        type: 'folder',
        parent_id: currentFolderId,
        path: newPath,
        user_id: user.user_id,
        url: `${API}/files/${user.user_id}`
      });
      
      const response = await axios.post(`${API}/files/${user.user_id}`, {
        name: newFolderName,
        type: 'folder',
        parent_id: currentFolderId,
        path: newPath
      });
      
      console.log('Folder created successfully:', response.data);
      setNewFolderName('');
      setShowNewFolderDialog(false);
      
      // Refresh the file list to show the new folder
      console.log('Refreshing files with currentFolderId:', currentFolderId, 'currentPath:', currentPath);
      await loadFiles(currentFolderId, currentPath);
    } catch (error) {
      console.error('Error creating folder:', error);
      console.error('Error response:', error.response?.data);
      
      let errorMessage = 'Error creating folder';
      if (error.response?.data?.detail) {
        if (error.response.data.detail === 'File already exists') {
          errorMessage = `A folder named "${newFolderName}" already exists in this location.`;
        } else {
          errorMessage = error.response.data.detail;
        }
      }
      
      alert(errorMessage);
    }
  };

  const deleteSelectedFiles = async () => {
    if (selectedFiles.length === 0) return;
    
    if (window.confirm(`Are you sure you want to move ${selectedFiles.length} item(s) to Recycle Bin?`)) {
      try {
        for (const fileId of selectedFiles) {
          await axios.delete(`${API}/files/${user.user_id}/${fileId}`);
        }
        setSelectedFiles([]);
        loadFiles(currentFolderId, currentPath);
      } catch (error) {
        console.error('Error deleting files:', error);
        alert('Error deleting files');
      }
    }
  };

  const renameFile = async (file, newName) => {
    if (!newName.trim() || newName === file.name) return;
    
    try {
      await axios.put(`${API}/files/${user.user_id}/${file.id}`, {
        name: newName
      });
      loadFiles(currentFolderId, currentPath);
    } catch (error) {
      console.error('Error renaming file:', error);
      alert('Error renaming file');
    }
  };

  const openFile = async (file) => {
    if (file.type === 'folder') {
      navigateToFolder(file);
    } else {
      // Open file in appropriate application
      const extension = file.file_extension?.toLowerCase();
      if (['txt', 'md', 'js', 'py', 'html', 'css', 'json'].includes(extension)) {
        // Open in text editor - we'll need to pass this up to the Desktop component
        if (window.openTextFile) {
          window.openTextFile(file);
        }
      } else {
        alert(`Cannot open ${extension} files yet`);
      }
    }
  };

  const selectFile = (fileId, isCtrlClick = false) => {
    if (isCtrlClick) {
      setSelectedFiles(prev => 
        prev.includes(fileId) 
          ? prev.filter(id => id !== fileId)
          : [...prev, fileId]
      );
    } else {
      setSelectedFiles([fileId]);
    }
  };

  const getPathSegments = () => {
    if (currentPath === '/') return [{ name: 'Root', path: '/' }];
    const parts = currentPath.split('/').filter(p => p);
    const segments = [{ name: 'Root', path: '/' }];
    let buildPath = '';
    parts.forEach(part => {
      buildPath += `/${part}`;
      segments.push({ name: part, path: buildPath });
    });
    return segments;
  };

  return (
    <div className="file-browser">
      {/* Toolbar */}
      <div className="file-browser-toolbar">
        <div className="nav-buttons">
          <button 
            onClick={navigateUp} 
            disabled={pathHistory.length <= 1}
            className="nav-btn"
            title="Up"
          >
            ⬆️
          </button>
          <button 
            onClick={() => loadFiles(currentFolderId, currentPath)}
            className="nav-btn"
            title="Refresh"
          >
            🔄
          </button>
        </div>
        
        <div className="path-bar">
          {getPathSegments().map((segment, index) => (
            <span key={index}>
              <button 
                className="path-segment"
                onClick={() => navigateToPath(segment.path)}
              >
                {segment.name}
              </button>
              {index < getPathSegments().length - 1 && <span className="path-separator"> › </span>}
            </span>
          ))}
        </div>

        <div className="view-controls">
          <button 
            className={`view-btn ${viewMode === 'list' ? 'active' : ''}`}
            onClick={() => setViewMode('list')}
            title="List View"
          >
            📋
          </button>
          <button 
            className={`view-btn ${viewMode === 'icons' ? 'active' : ''}`}
            onClick={() => setViewMode('icons')}
            title="Icon View"
          >
            🔲
          </button>
        </div>
      </div>

      {/* Action Buttons */}
      <div className="file-actions">
        <button 
          onClick={() => setShowNewFolderDialog(true)} 
          className="action-btn"
        >
          📁 New Folder
        </button>
        <button 
          onClick={deleteSelectedFiles} 
          disabled={selectedFiles.length === 0}
          className="action-btn"
        >
          🗑️ Delete Selected ({selectedFiles.length})
        </button>
      </div>

      {/* File List */}
      <div className={`file-list ${viewMode}`}>
        {loading ? (
          <div className="loading">Loading...</div>
        ) : files.length === 0 ? (
          <div className="empty-folder">This folder is empty</div>
        ) : (
          viewMode === 'list' ? (
            <>
              <div className="file-list-header">
                <div className="col-name">Name</div>
                <div className="col-type">Type</div>
                <div className="col-size">Size</div>
                <div className="col-modified">Modified</div>
              </div>
              {files.map(file => (
                <div 
                  key={file.id} 
                  className={`file-item ${selectedFiles.includes(file.id) ? 'selected' : ''}`}
                  onClick={(e) => selectFile(file.id, e.ctrlKey)}
                  onDoubleClick={() => openFile(file)}
                >
                  <div className="col-name">
                    <span className="file-icon">
                      {file.type === 'folder' ? '📁' : getFileIcon(file.file_extension)}
                    </span>
                    <span className="file-name">{file.name}</span>
                  </div>
                  <div className="col-type">{file.type === 'folder' ? 'Folder' : (file.file_extension || 'File')}</div>
                  <div className="col-size">{file.type === 'folder' ? '--' : `${file.size} bytes`}</div>
                  <div className="col-modified">{new Date(file.modified_at).toLocaleDateString()}</div>
                </div>
              ))}
            </>
          ) : (
            <div className="file-grid">
              {files.map(file => (
                <div 
                  key={file.id} 
                  className={`file-icon-item ${selectedFiles.includes(file.id) ? 'selected' : ''}`}
                  onClick={(e) => selectFile(file.id, e.ctrlKey)}
                  onDoubleClick={() => openFile(file)}
                >
                  <div className="large-file-icon">
                    {file.type === 'folder' ? '📁' : getFileIcon(file.file_extension)}
                  </div>
                  <div className="file-name">{file.name}</div>
                </div>
              ))}
            </div>
          )
        )}
      </div>

      {/* New Folder Dialog */}
      {showNewFolderDialog && (
        <div className="modal-overlay">
          <div className="modal-dialog">
            <h3>Create New Folder</h3>
            <input 
              type="text" 
              value={newFolderName}
              onChange={(e) => setNewFolderName(e.target.value)}
              placeholder="Folder name"
              onKeyPress={(e) => e.key === 'Enter' && createFolder()}
              autoFocus
            />
            <div className="modal-buttons">
              <button onClick={createFolder} disabled={!newFolderName.trim()}>Create</button>
              <button onClick={() => {
                setShowNewFolderDialog(false);
                setNewFolderName('');
              }}>Cancel</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

// Web Browser App (keeping original)
const WebBrowser = () => {
  const [url, setUrl] = useState('https://www.google.com');
  const [currentUrl, setCurrentUrl] = useState('https://www.google.com');

  const navigate = () => {
    setCurrentUrl(url);
  };

  const handleKeyPress = (e) => {
    if (e.key === 'Enter') {
      navigate();
    }
  };

  return (
    <div className="web-browser">
      <div className="browser-toolbar">
        <input 
          type="text" 
          value={url} 
          onChange={(e) => setUrl(e.target.value)}
          onKeyPress={handleKeyPress}
          className="url-input"
          placeholder="Enter URL..."
        />
        <button onClick={navigate} className="go-btn">Go</button>
      </div>
      <iframe 
        src={currentUrl} 
        className="browser-frame"
        title="Web Browser"
      />
    </div>
  );
};

// Classic Solitaire Game
const Solitaire = () => {
  const [cards, setCards] = useState([]);
  const [draggedCard, setDraggedCard] = useState(null);
  const [score, setScore] = useState(0);
  const [moves, setMoves] = useState(0);
  const [gameWon, setGameWon] = useState(false);

  const suits = ['♠', '♥', '♦', '♣'];
  const values = ['A', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K'];

  const initializeGame = () => {
    const deck = [];
    suits.forEach(suit => {
      values.forEach(value => {
        deck.push({
          id: `${suit}-${value}`,
          suit,
          value,
          color: suit === '♥' || suit === '♦' ? 'red' : 'black',
          faceUp: false
        });
      });
    });
    
    // Shuffle deck
    for (let i = deck.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [deck[i], deck[j]] = [deck[j], deck[i]];
    }

    setCards(deck);
    setScore(0);
    setMoves(0);
    setGameWon(false);
  };

  useEffect(() => {
    initializeGame();
  }, []);

  return (
    <div className="solitaire-game">
      <div className="solitaire-header">
        <div className="game-stats">
          <span>Score: {score}</span>
          <span>Moves: {moves}</span>
        </div>
        <button onClick={initializeGame} className="new-game-btn">New Game</button>
      </div>
      <div className="solitaire-board">
        <div className="card-placeholder">
          <div className="card back">🂠</div>
        </div>
        <div className="game-message">
          {gameWon ? '🎉 You Won! 🎉' : 'Classic Solitaire - Coming Soon!'}
        </div>
      </div>
    </div>
  );
};

// Classic Minesweeper Game  
const Minesweeper = () => {
  const [board, setBoard] = useState([]);
  const [gameState, setGameState] = useState('playing'); // 'playing', 'won', 'lost'
  const [mineCount, setMineCount] = useState(10);
  const [flagCount, setFlagCount] = useState(0);
  const [timer, setTimer] = useState(0);
  const [gameStarted, setGameStarted] = useState(false);

  const ROWS = 9;
  const COLS = 9;
  const MINES = 10;

  const initializeBoard = () => {
    const newBoard = Array(ROWS).fill().map(() => 
      Array(COLS).fill().map(() => ({
        isMine: false,
        isRevealed: false,
        isFlagged: false,
        neighborMines: 0
      }))
    );

    // Place mines randomly
    let minesPlaced = 0;
    while (minesPlaced < MINES) {
      const row = Math.floor(Math.random() * ROWS);
      const col = Math.floor(Math.random() * COLS);
      if (!newBoard[row][col].isMine) {
        newBoard[row][col].isMine = true;
        minesPlaced++;
      }
    }

    // Calculate neighbor mine counts
    for (let row = 0; row < ROWS; row++) {
      for (let col = 0; col < COLS; col++) {
        if (!newBoard[row][col].isMine) {
          let count = 0;
          for (let r = -1; r <= 1; r++) {
            for (let c = -1; c <= 1; c++) {
              const newRow = row + r;
              const newCol = col + c;
              if (newRow >= 0 && newRow < ROWS && newCol >= 0 && newCol < COLS) {
                if (newBoard[newRow][newCol].isMine) count++;
              }
            }
          }
          newBoard[row][col].neighborMines = count;
        }
      }
    }

    setBoard(newBoard);
    setGameState('playing');
    setFlagCount(0);
    setTimer(0);
    setGameStarted(false);
  };

  const handleCellClick = (row, col) => {
    if (gameState !== 'playing' || board[row][col].isRevealed || board[row][col].isFlagged) {
      return;
    }

    if (!gameStarted) {
      setGameStarted(true);
    }

    const newBoard = [...board];
    
    if (newBoard[row][col].isMine) {
      setGameState('lost');
      // Reveal all mines
      for (let r = 0; r < ROWS; r++) {
        for (let c = 0; c < COLS; c++) {
          if (newBoard[r][c].isMine) {
            newBoard[r][c].isRevealed = true;
          }
        }
      }
    } else {
      revealCell(newBoard, row, col);
      checkWin(newBoard);
    }

    setBoard(newBoard);
  };

  const revealCell = (board, row, col) => {
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS || 
        board[row][col].isRevealed || board[row][col].isFlagged) {
      return;
    }

    board[row][col].isRevealed = true;

    if (board[row][col].neighborMines === 0) {
      for (let r = -1; r <= 1; r++) {
        for (let c = -1; c <= 1; c++) {
          revealCell(board, row + r, col + c);
        }
      }
    }
  };

  const handleRightClick = (e, row, col) => {
    e.preventDefault();
    if (gameState !== 'playing' || board[row][col].isRevealed) {
      return;
    }

    const newBoard = [...board];
    newBoard[row][col].isFlagged = !newBoard[row][col].isFlagged;
    setBoard(newBoard);
    setFlagCount(flagCount + (newBoard[row][col].isFlagged ? 1 : -1));
  };

  const checkWin = (board) => {
    let revealedCount = 0;
    for (let row = 0; row < ROWS; row++) {
      for (let col = 0; col < COLS; col++) {
        if (board[row][col].isRevealed && !board[row][col].isMine) {
          revealedCount++;
        }
      }
    }
    if (revealedCount === ROWS * COLS - MINES) {
      setGameState('won');
    }
  };

  useEffect(() => {
    initializeBoard();
  }, []);

  useEffect(() => {
    let interval;
    if (gameStarted && gameState === 'playing') {
      interval = setInterval(() => {
        setTimer(t => t + 1);
      }, 1000);
    }
    return () => clearInterval(interval);
  }, [gameStarted, gameState]);

  const getCellContent = (cell) => {
    if (cell.isFlagged) return '🚩';
    if (!cell.isRevealed) return '';
    if (cell.isMine) return '💣';
    if (cell.neighborMines === 0) return '';
    return cell.neighborMines;
  };

  const getCellClass = (cell) => {
    let className = 'mine-cell';
    if (cell.isRevealed) {
      className += ' revealed';
      if (cell.isMine) className += ' mine';
    }
    if (cell.isFlagged) className += ' flagged';
    return className;
  };

  return (
    <div className="minesweeper-game">
      <div className="minesweeper-header">
        <div className="mine-counter">💣 {MINES - flagCount}</div>
        <button onClick={initializeBoard} className="new-game-btn">
          {gameState === 'lost' ? '😵' : gameState === 'won' ? '😎' : '🙂'}
        </button>
        <div className="timer">⏱️ {timer}</div>
      </div>
      <div className="minesweeper-board">
        {board.map((row, rowIndex) => (
          <div key={rowIndex} className="mine-row">
            {row.map((cell, colIndex) => (
              <div
                key={`${rowIndex}-${colIndex}`}
                className={getCellClass(cell)}
                onClick={() => handleCellClick(rowIndex, colIndex)}
                onContextMenu={(e) => handleRightClick(e, rowIndex, colIndex)}
              >
                {getCellContent(cell)}
              </div>
            ))}
          </div>
        ))}
      </div>
      {gameState !== 'playing' && (
        <div className="game-over-message">
          {gameState === 'won' ? '🎉 You Won! 🎉' : '💥 Game Over! 💥'}
        </div>
      )}
    </div>
  );
};

// Classic Snake Game
const Snake = () => {
  const [snake, setSnake] = useState([{x: 10, y: 10}]);
  const [food, setFood] = useState({x: 15, y: 15});
  const [direction, setDirection] = useState({x: 0, y: -1});
  const [gameRunning, setGameRunning] = useState(false);
  const [score, setScore] = useState(0);
  const [gameOver, setGameOver] = useState(false);

  const BOARD_SIZE = 20;

  const generateFood = () => {
    const newFood = {
      x: Math.floor(Math.random() * BOARD_SIZE),
      y: Math.floor(Math.random() * BOARD_SIZE)
    };
    setFood(newFood);
  };

  const startGame = () => {
    setSnake([{x: 10, y: 10}]);
    setDirection({x: 0, y: -1});
    setScore(0);
    setGameOver(false);
    setGameRunning(true);
    generateFood();
  };

  const stopGame = () => {
    setGameRunning(false);
    setGameOver(true);
  };

  useEffect(() => {
    const handleKeyPress = (e) => {
      if (!gameRunning) return;
      
      switch(e.key) {
        case 'ArrowUp':
          if (direction.y === 0) setDirection({x: 0, y: -1});
          break;
        case 'ArrowDown':
          if (direction.y === 0) setDirection({x: 0, y: 1});
          break;
        case 'ArrowLeft':
          if (direction.x === 0) setDirection({x: -1, y: 0});
          break;
        case 'ArrowRight':
          if (direction.x === 0) setDirection({x: 1, y: 0});
          break;
      }
    };

    window.addEventListener('keydown', handleKeyPress);
    return () => window.removeEventListener('keydown', handleKeyPress);
  }, [direction, gameRunning]);

  useEffect(() => {
    if (!gameRunning) return;

    const gameLoop = setInterval(() => {
      setSnake(currentSnake => {
        const newSnake = [...currentSnake];
        const head = {...newSnake[0]};
        
        head.x += direction.x;
        head.y += direction.y;

        // Check wall collision
        if (head.x < 0 || head.x >= BOARD_SIZE || head.y < 0 || head.y >= BOARD_SIZE) {
          stopGame();
          return currentSnake;
        }

        // Check self collision
        if (newSnake.some(segment => segment.x === head.x && segment.y === head.y)) {
          stopGame();
          return currentSnake;
        }

        newSnake.unshift(head);

        // Check food collision
        if (head.x === food.x && head.y === food.y) {
          setScore(s => s + 10);
          generateFood();
        } else {
          newSnake.pop();
        }

        return newSnake;
      });
    }, 150);

    return () => clearInterval(gameLoop);
  }, [direction, food, gameRunning]);

  const isSnakeSegment = (x, y) => {
    return snake.some(segment => segment.x === x && segment.y === y);
  };

  const isFood = (x, y) => {
    return food.x === x && food.y === y;
  };

  return (
    <div className="snake-game">
      <div className="snake-header">
        <div className="score">Score: {score}</div>
        <button onClick={startGame} className="new-game-btn">
          {gameRunning ? 'Restart' : 'Start Game'}
        </button>
      </div>
      <div className="snake-board">
        {Array(BOARD_SIZE).fill().map((_, y) => (
          <div key={y} className="snake-row">
            {Array(BOARD_SIZE).fill().map((_, x) => (
              <div
                key={`${x}-${y}`}
                className={`snake-cell ${
                  isSnakeSegment(x, y) ? 'snake' : 
                  isFood(x, y) ? 'food' : ''
                }`}
              >
                {isSnakeSegment(x, y) ? '█' : isFood(x, y) ? '🍎' : ''}
              </div>
            ))}
          </div>
        ))}
      </div>
      <div className="snake-controls">
        <div>Use arrow keys to control the snake</div>
        {gameOver && <div className="game-over">Game Over! Score: {score}</div>}
      </div>
    </div>
  );
};

// Classic Tic Tac Toe Game
const TicTacToe = () => {
  const [board, setBoard] = useState(Array(9).fill(null));
  const [isXNext, setIsXNext] = useState(true);
  const [winner, setWinner] = useState(null);
  const [gameMode, setGameMode] = useState('human'); // 'human' or 'computer'

  const calculateWinner = (squares) => {
    const lines = [
      [0, 1, 2], [3, 4, 5], [6, 7, 8],
      [0, 3, 6], [1, 4, 7], [2, 5, 8],
      [0, 4, 8], [2, 4, 6]
    ];
    
    for (let line of lines) {
      const [a, b, c] = line;
      if (squares[a] && squares[a] === squares[b] && squares[a] === squares[c]) {
        return squares[a];
      }
    }
    return null;
  };

  const makeComputerMove = (squares) => {
    const availableMoves = squares.map((square, index) => square === null ? index : null).filter(val => val !== null);
    if (availableMoves.length === 0) return squares;
    
    const randomMove = availableMoves[Math.floor(Math.random() * availableMoves.length)];
    const newSquares = [...squares];
    newSquares[randomMove] = 'O';
    return newSquares;
  };

  const handleClick = (index) => {
    if (board[index] || winner) return;

    const newBoard = [...board];
    newBoard[index] = isXNext ? 'X' : 'O';
    
    const gameWinner = calculateWinner(newBoard);
    setWinner(gameWinner);
    
    if (gameMode === 'computer' && !gameWinner && isXNext) {
      // Player move (X)
      setBoard(newBoard);
      setIsXNext(false);
      
      // Computer move (O) after a short delay
      setTimeout(() => {
        const computerBoard = makeComputerMove(newBoard);
        const computerWinner = calculateWinner(computerBoard);
        setBoard(computerBoard);
        setWinner(computerWinner);
        setIsXNext(true);
      }, 500);
    } else {
      setBoard(newBoard);
      setIsXNext(!isXNext);
    }
  };

  const resetGame = () => {
    setBoard(Array(9).fill(null));
    setIsXNext(true);
    setWinner(null);
  };

  const renderSquare = (index) => (
    <button
      className="tic-tac-square"
      onClick={() => handleClick(index)}
      disabled={board[index] !== null || winner !== null}
    >
      {board[index]}
    </button>
  );

  const isDraw = board.every(square => square !== null) && !winner;

  return (
    <div className="tic-tac-toe-game">
      <div className="tic-tac-header">
        <div className="game-mode">
          <label>
            <input
              type="radio"
              checked={gameMode === 'human'}
              onChange={() => {
                setGameMode('human');
                resetGame();
              }}
            />
            2 Players
          </label>
          <label>
            <input
              type="radio"
              checked={gameMode === 'computer'}
              onChange={() => {
                setGameMode('computer');
                resetGame();
              }}
            />
            vs Computer
          </label>
        </div>
        <button onClick={resetGame} className="new-game-btn">New Game</button>
      </div>
      
      <div className="tic-tac-board">
        <div className="board-row">
          {renderSquare(0)}
          {renderSquare(1)}
          {renderSquare(2)}
        </div>
        <div className="board-row">
          {renderSquare(3)}
          {renderSquare(4)}
          {renderSquare(5)}
        </div>
        <div className="board-row">
          {renderSquare(6)}
          {renderSquare(7)}
          {renderSquare(8)}
        </div>
      </div>
      
      <div className="tic-tac-status">
        {winner ? (
          <div className="winner">🎉 {winner} Wins! 🎉</div>
        ) : isDraw ? (
          <div className="draw">It's a Draw!</div>
        ) : (
          <div className="next-player">
            Next player: {gameMode === 'computer' ? 'Your turn (X)' : isXNext ? 'X' : 'O'}
          </div>
        )}
      </div>
    </div>
  );
};

// Memory Card Game
const MemoryGame = () => {
  const [cards, setCards] = useState([]);
  const [flippedCards, setFlippedCards] = useState([]);
  const [matchedCards, setMatchedCards] = useState([]);
  const [moves, setMoves] = useState(0);
  const [gameWon, setGameWon] = useState(false);

  const cardEmojis = ['🎮', '🎯', '🎲', '🃏', '🎪', '🎭', '🎨', '🎵'];

  const initializeGame = () => {
    const gameCards = [...cardEmojis, ...cardEmojis].map((emoji, index) => ({
      id: index,
      emoji,
      isFlipped: false,
      isMatched: false
    }));
    
    // Shuffle cards
    for (let i = gameCards.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [gameCards[i], gameCards[j]] = [gameCards[j], gameCards[i]];
    }
    
    setCards(gameCards);
    setFlippedCards([]);
    setMatchedCards([]);
    setMoves(0);
    setGameWon(false);
  };

  const handleCardClick = (cardId) => {
    if (flippedCards.length >= 2 || flippedCards.includes(cardId) || matchedCards.includes(cardId)) {
      return;
    }

    const newFlippedCards = [...flippedCards, cardId];
    setFlippedCards(newFlippedCards);

    if (newFlippedCards.length === 2) {
      setMoves(moves + 1);
      
      const card1 = cards.find(card => card.id === newFlippedCards[0]);
      const card2 = cards.find(card => card.id === newFlippedCards[1]);
      
      if (card1.emoji === card2.emoji) {
        // Match found
        setTimeout(() => {
          setMatchedCards([...matchedCards, ...newFlippedCards]);
          setFlippedCards([]);
          
          if (matchedCards.length + 2 === cards.length) {
            setGameWon(true);
          }
        }, 500);
      } else {
        // No match
        setTimeout(() => {
          setFlippedCards([]);
        }, 1000);
      }
    }
  };

  useEffect(() => {
    initializeGame();
  }, []);

  return (
    <div className="memory-game">
      <div className="memory-header">
        <div className="game-stats">
          <span>Moves: {moves}</span>
          <span>Pairs: {matchedCards.length / 2}/{cardEmojis.length}</span>
        </div>
        <button onClick={initializeGame} className="new-game-btn">New Game</button>
      </div>
      
      <div className="memory-board">
        {cards.map(card => (
          <div
            key={card.id}
            className={`memory-card ${
              flippedCards.includes(card.id) || matchedCards.includes(card.id) ? 'flipped' : ''
            }`}
            onClick={() => handleCardClick(card.id)}
          >
            <div className="card-front">?</div>
            <div className="card-back">{card.emoji}</div>
          </div>
        ))}
      </div>
      
      {gameWon && (
        <div className="game-won-message">
          🎉 Congratulations! You won in {moves} moves! 🎉
        </div>
      )}
    </div>
  );
};

// Matrix Rain Screensaver Component
const MatrixRain = ({ isActive, onDeactivate }) => {
  const canvasRef = useRef(null);
  const animationRef = useRef(null);

  useEffect(() => {
    if (!isActive) return;

    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');
    
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;

    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%^&*()';
    const matrix = chars.split('');
    const drops = [];
    const fontSize = 10;
    const columns = canvas.width / fontSize;

    // Initialize drops
    for (let x = 0; x < columns; x++) {
      drops[x] = 1;
    }

    const draw = () => {
      ctx.fillStyle = 'rgba(0, 0, 0, 0.04)';
      ctx.fillRect(0, 0, canvas.width, canvas.height);

      ctx.fillStyle = '#0F0';
      ctx.font = fontSize + 'px monospace';

      for (let i = 0; i < drops.length; i++) {
        const text = matrix[Math.floor(Math.random() * matrix.length)];
        ctx.fillText(text, i * fontSize, drops[i] * fontSize);

        if (drops[i] * fontSize > canvas.height && Math.random() > 0.975) {
          drops[i] = 0;
        }
        drops[i]++;
      }
    };

    const animate = () => {
      draw();
      animationRef.current = requestAnimationFrame(animate);
    };

    animate();

    const handleClick = () => {
      onDeactivate();
    };

    const handleKeyPress = () => {
      onDeactivate();
    };

    canvas.addEventListener('click', handleClick);
    window.addEventListener('keydown', handleKeyPress);

    return () => {
      if (animationRef.current) {
        cancelAnimationFrame(animationRef.current);
      }
      canvas.removeEventListener('click', handleClick);
      window.removeEventListener('keydown', handleKeyPress);
    };
  }, [isActive, onDeactivate]);

  if (!isActive) return null;

  return (
    <div className="matrix-screensaver">
      <canvas ref={canvasRef} />
      <div className="screensaver-hint">
        Click anywhere or press any key to exit screensaver
      </div>
    </div>
  );
};

// Particle Background Component
const ParticleBackground = ({ enabled = true }) => {
  const canvasRef = useRef(null);
  const animationRef = useRef(null);
  const particlesRef = useRef([]);

  useEffect(() => {
    if (!enabled) return;

    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');
    
    const resizeCanvas = () => {
      canvas.width = window.innerWidth;
      canvas.height = window.innerHeight;
    };

    resizeCanvas();
    window.addEventListener('resize', resizeCanvas);

    // Create particles
    const createParticles = () => {
      const particles = [];
      const numParticles = Math.floor((canvas.width * canvas.height) / 15000);
      
      for (let i = 0; i < numParticles; i++) {
        particles.push({
          x: Math.random() * canvas.width,
          y: Math.random() * canvas.height,
          size: Math.random() * 2 + 0.5,
          speedX: (Math.random() - 0.5) * 0.5,
          speedY: (Math.random() - 0.5) * 0.5,
          opacity: Math.random() * 0.5 + 0.2,
          color: `hsl(${Math.random() * 60 + 180}, 50%, 70%)` // Cyan/blue hues
        });
      }
      return particles;
    };

    particlesRef.current = createParticles();

    const animate = () => {
      ctx.clearRect(0, 0, canvas.width, canvas.height);

      particlesRef.current.forEach(particle => {
        // Update position
        particle.x += particle.speedX;
        particle.y += particle.speedY;

        // Wrap around edges
        if (particle.x < 0) particle.x = canvas.width;
        if (particle.x > canvas.width) particle.x = 0;
        if (particle.y < 0) particle.y = canvas.height;
        if (particle.y > canvas.height) particle.y = 0;

        // Draw particle
        ctx.save();
        ctx.globalAlpha = particle.opacity;
        ctx.fillStyle = particle.color;
        ctx.beginPath();
        ctx.arc(particle.x, particle.y, particle.size, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();

        // Add subtle glow effect
        ctx.save();
        ctx.globalAlpha = particle.opacity * 0.3;
        ctx.fillStyle = particle.color;
        ctx.beginPath();
        ctx.arc(particle.x, particle.y, particle.size * 3, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();
      });

      animationRef.current = requestAnimationFrame(animate);
    };

    animate();

    return () => {
      if (animationRef.current) {
        cancelAnimationFrame(animationRef.current);
      }
      window.removeEventListener('resize', resizeCanvas);
    };
  }, [enabled]);

  if (!enabled) return null;

  return <canvas ref={canvasRef} className="particle-background" />;
};

// RetroOS Boot Screen Component
const BootScreen = ({ onComplete }) => {
  const [bootStage, setBootStage] = useState(0);
  const [bootText, setBootText] = useState('');

  const bootMessages = [
    'RetroOS v1.0 Starting...',
    'Loading system drivers...',
    'Initializing desktop environment...',
    'Loading applications...',
    'Connecting to matrix...',
    'Boot sequence complete!'
  ];

  useEffect(() => {
    const bootSequence = async () => {
      for (let i = 0; i < bootMessages.length; i++) {
        setBootStage(i);
        setBootText(bootMessages[i]);
        await new Promise(resolve => setTimeout(resolve, 800));
      }
      setTimeout(onComplete, 500);
    };

    bootSequence();
  }, [onComplete]);

  return (
    <div className="boot-screen">
      <div className="boot-content">
        <div className="boot-logo">
          <div className="logo-text">RetroOS</div>
          <div className="logo-version">Enhanced Edition</div>
        </div>
        
        <div className="boot-progress">
          <div className="boot-message">{bootText}</div>
          <div className="progress-bar">
            <div 
              className="progress-fill" 
              style={{ width: `${(bootStage / (bootMessages.length - 1)) * 100}%` }}
            />
          </div>
        </div>

        <div className="boot-effects">
          {[...Array(20)].map((_, i) => (
            <div 
              key={i} 
              className="boot-particle"
              style={{
                left: `${Math.random() * 100}%`,
                animationDelay: `${Math.random() * 2}s`,
                animationDuration: `${2 + Math.random() * 2}s`
              }}
            />
          ))}
        </div>
      </div>
    </div>
  );
};

// Holographic Theme Manager
const HolographicTheme = ({ children, enabled = true }) => {
  if (!enabled) return children;

  return (
    <div className="holographic-theme">
      {children}
      <div className="holographic-overlay" />
    </div>
  );
};
  const games = [
    { 
      id: 'solitaire',
      name: 'Solitaire', 
      icon: '🃏', 
      description: 'Classic Klondike Solitaire',
      component: 'solitaire'
    },
    { 
      id: 'minesweeper',
      name: 'Minesweeper', 
      icon: '💣', 
      description: 'Find all the mines',
      component: 'minesweeper'
    },
    { 
      id: 'snake',
      name: 'Snake', 
      icon: '🐍', 
      description: 'Classic Snake game',
      component: 'snake'
    },
    { 
      id: 'tictactoe',
      name: 'Tic Tac Toe', 
      icon: '⭕', 
      description: 'X\'s and O\'s',
      component: 'tictactoe'
    },
    { 
      id: 'memory',
      name: 'Memory', 
      icon: '🧠', 
      description: 'Match the pairs',
      component: 'memory'
    },
    { 
      id: 'web-games',
      name: 'Web Games', 
      icon: '🌐', 
      description: 'Online retro games',
      component: 'webgames'
    }
  ];

  return (
    <div className="games-launcher">
      <div className="games-header">
        <h3>🎮 RetroOS Games</h3>
        <p>Choose a game to play:</p>
      </div>
      <div className="games-grid">
        {games.map((game) => (
          <div 
            key={game.id} 
            className="game-tile"
            onClick={() => onOpenGame(game)}
          >
            <div className="game-icon">{game.icon}</div>
            <div className="game-info">
              <div className="game-name">{game.name}</div>
              <div className="game-description">{game.description}</div>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
};

// Game Window Component (keeping original)
const GameWindow = ({ game }) => (
  <iframe 
    src={game.url} 
    className="game-frame"
    title={game.name}
  />
);

// Login Component (keeping original)
const Login = ({ onLogin, onRegister }) => {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [isRegistering, setIsRegistering] = useState(false);

  const handleSubmit = async (e) => {
    e.preventDefault();
    const endpoint = isRegistering ? 'register' : 'login';
    
    try {
      const response = await axios.post(`${API}/${endpoint}`, {
        username,
        password
      });
      
      if (isRegistering) {
        alert('Registration successful! Please log in.');
        setIsRegistering(false);
      } else {
        onLogin(response.data);
      }
    } catch (error) {
      alert(`${isRegistering ? 'Registration' : 'Login'} failed: ${error.response?.data?.detail || error.message}`);
    }
  };

  return (
    <div className="login-screen">
      <div className="login-box">
        <h2>{isRegistering ? 'Register' : 'Login'} to RetroOS</h2>
        <form onSubmit={handleSubmit}>
          <input
            type="text"
            placeholder="Username"
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            required
          />
          <input
            type="password"
            placeholder="Password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            required
          />
          <button type="submit">
            {isRegistering ? 'Register' : 'Login'}
          </button>
        </form>
        <button 
          onClick={() => setIsRegistering(!isRegistering)}
          className="switch-mode-btn"
        >
          {isRegistering ? 'Already have an account? Login' : 'Need an account? Register'}
        </button>
      </div>
    </div>
  );
};

// Main Desktop Component
const Desktop = ({ user, onLogout }) => {
  const [windows, setWindows] = useState([]);
  const [nextZIndex, setNextZIndex] = useState(1);
  const [currentTime, setCurrentTime] = useState(new Date());
  const [startMenuOpen, setStartMenuOpen] = useState(false);
  const [contextMenuOpen, setContextMenuOpen] = useState(false);
  const [contextMenuPosition, setContextMenuPosition] = useState({ x: 0, y: 0 });
  const [wallpaper, setWallpaper] = useState(user?.desktop_settings?.wallpaper || 'classic_clouds');
  const [settings, setSettings] = useState({
    taskbarPosition: user?.desktop_settings?.taskbar_settings?.position || 'bottom',
    iconSize: user?.desktop_settings?.iconSize || 'normal',
    theme: user?.desktop_settings?.theme || 'classic',
    windowAnimations: user?.desktop_settings?.windowAnimations !== false
  });

  useEffect(() => {
    const timer = setInterval(() => {
      setCurrentTime(new Date());
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  useEffect(() => {
    setWallpaper(user?.desktop_settings?.wallpaper || 'classic_clouds');
    setSettings({
      taskbarPosition: user?.desktop_settings?.taskbar_settings?.position || 'bottom',
      iconSize: user?.desktop_settings?.iconSize || 'normal',
      theme: user?.desktop_settings?.theme || 'classic',
      windowAnimations: user?.desktop_settings?.windowAnimations !== false
    });
  }, [user?.desktop_settings]);

  const openWindow = (type, title, props = {}) => {
    // Check if window is already open
    const existingWindow = windows.find(w => w.type === type);
    if (existingWindow) {
      // Bring existing window to front
      bringToFront(existingWindow.id);
      return;
    }

    const newWindow = {
      id: Date.now(),
      type,
      title,
      zIndex: nextZIndex,
      isMaximized: false,
      isMinimized: false,
      ...props
    };
    setWindows([...windows, newWindow]);
    setNextZIndex(nextZIndex + 1);
  };

  const closeWindow = (id) => {
    setWindows(windows.filter(w => w.id !== id));
  };

  const minimizeWindow = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, isMinimized: true } : w
    ));
  };

  const maximizeWindow = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, isMaximized: !w.isMaximized } : w
    ));
  };

  const restoreWindow = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, isMinimized: false, zIndex: nextZIndex } : w
    ));
    setNextZIndex(nextZIndex + 1);
  };

  const bringToFront = (id) => {
    setWindows(windows.map(w => 
      w.id === id ? { ...w, zIndex: nextZIndex } : w
    ));
    setNextZIndex(nextZIndex + 1);
  };

  const handleDesktopRightClick = (e) => {
    e.preventDefault();
    setContextMenuPosition({ x: e.clientX, y: e.clientY });
    setContextMenuOpen(true);
    setStartMenuOpen(false);
  };

  const handleDesktopClick = (e) => {
    if (e.target.classList.contains('desktop') || e.target.classList.contains('desktop-icons')) {
      setStartMenuOpen(false);
      setContextMenuOpen(false);
    }
  };

  const handleContextAction = async (action) => {
    switch (action) {
      case 'new-folder':
        const folderName = prompt('Enter folder name:');
        if (folderName) {
          try {
            await axios.post(`${API}/files/${user.user_id}`, {
              name: folderName,
              type: 'folder',
              path: `/Desktop/${folderName}`
            });
            alert('Folder created successfully!');
          } catch (error) {
            console.error('Error creating folder:', error);
          }
        }
        break;
      case 'new-text':
        openWindow('texteditor', 'New Text Document');
        break;
      case 'refresh':
        window.location.reload();
        break;
      case 'properties':
        openWindow('systeminfo', 'Desktop Properties');
        break;
      case 'arrange-icons':
        alert('Icons arranged!');
        break;
      case 'view-large':
      case 'view-small':
        const newSize = action === 'view-large' ? 'large' : 'small';
        updateDesktopSettings({ ...settings, iconSize: newSize });
        break;
    }
  };

  const updateDesktopSettings = async (newSettings) => {
    try {
      const updatedSettings = {
        ...user.desktop_settings,
        ...newSettings,
        taskbar_settings: { position: newSettings.taskbarPosition || settings.taskbarPosition }
      };
      
      await axios.put(`${API}/user/${user.user_id}/desktop-settings`, updatedSettings);
      setSettings(newSettings);
      
      if (newSettings.wallpaper) {
        setWallpaper(newSettings.wallpaper);
      }
    } catch (error) {
      console.error('Error updating settings:', error);
    }
  };

  const shutDown = () => {
    if (window.confirm('Are you sure you want to shut down RetroOS?')) {
      onLogout();
    }
  };

  const renderWindowContent = (window) => {
    switch (window.type) {
      case 'calculator':
        return <Calculator />;
      case 'texteditor':
        return <TextEditor user={user} initialFile={window.file} />;
      case 'filebrowser':
        return <FileBrowser user={user} />;
      case 'webbrowser':
        return <WebBrowser />;
      case 'games':
        return <GamesLauncher onOpenGame={(game) => {
          if (game.component === 'webgames') {
            // For web games, use the old iframe system
            openWindow('game', game.name, { game: { url: 'https://www.google.com/search?q=retro+games' } });
          } else {
            // For built-in games, open the game component
            openWindow(game.component, game.name);
          }
        }} />;
      case 'solitaire':
        return <Solitaire />;
      case 'minesweeper':
        return <Minesweeper />;
      case 'snake':
        return <Snake />;
      case 'tictactoe':
        return <TicTacToe />;
      case 'memory':
        return <MemoryGame />;
      case 'game':
        return <GameWindow game={window.game} />;
      case 'paint':
        return <Paint />;
      case 'controlpanel':
        return <ControlPanel user={user} onUpdateSettings={updateDesktopSettings} />;
      case 'systeminfo':
        return <SystemInfo />;
      case 'run':
        return <RunDialog onOpenApp={openWindow} />;
      case 'recyclebin':
        return <RecycleBin user={user} />;
      case 'taskmanager':
        return <TaskManager windows={windows} onCloseWindow={closeWindow} />;
      case 'explorer':
        return <Explorer user={user} onOpenFile={(file) => openWindow('texteditor', file.name, { file })} />;
      default:
        return <div>Unknown application</div>;
    }
  };

  // Set up global file opening function for File Browser integration
  useEffect(() => {
    window.openTextFile = (file) => {
      const windowTitle = file.name;
      openWindow('texteditor', windowTitle, { file });
    };
    
    return () => {
      delete window.openTextFile;
    };
  }, []);

  const openFileInTextEditor = (file) => {
    const windowTitle = file.name;
    openWindow('texteditor', windowTitle, { file });
  };

  const getTaskbarStyle = () => {
    const position = settings.taskbarPosition;
    const baseStyle = {
      position: 'fixed',
      background: '#c0c0c0',
      border: '1px solid #808080',
      display: 'flex',
      alignItems: 'center',
      zIndex: 9999
    };

    switch (position) {
      case 'top':
        return { ...baseStyle, top: 0, left: 0, right: 0, height: '40px', borderTop: 'none' };
      case 'left':
        return { ...baseStyle, top: 0, left: 0, bottom: 0, width: '200px', flexDirection: 'column', borderLeft: 'none' };
      case 'right':
        return { ...baseStyle, top: 0, right: 0, bottom: 0, width: '200px', flexDirection: 'column', borderRight: 'none' };
      default: // bottom
        return { ...baseStyle, bottom: 0, left: 0, right: 0, height: '40px', borderBottom: 'none' };
    }
  };

  const getDesktopStyle = () => {
    const position = settings.taskbarPosition;
    const baseStyle = { height: '100vh', position: 'relative', overflow: 'hidden' };

    switch (position) {
      case 'top':
        return { ...baseStyle, paddingTop: '40px', height: 'calc(100vh - 40px)', marginTop: '40px' };
      case 'left':
        return { ...baseStyle, paddingLeft: '200px', width: 'calc(100vw - 200px)', marginLeft: '200px' };
      case 'right':
        return { ...baseStyle, paddingRight: '200px', width: 'calc(100vw - 200px)' };
      default: // bottom
        return { ...baseStyle, paddingBottom: '40px' };
    }
  };

  return (
    <div 
      className={`desktop wallpaper-${wallpaper} theme-${settings.theme} ${settings.windowAnimations ? 'animations-enabled' : ''}`}
      style={getDesktopStyle()}
      onContextMenu={handleDesktopRightClick}
      onClick={handleDesktopClick}
    >
      {/* Desktop Icons */}
      <div className={`desktop-icons icon-size-${settings.iconSize}`}>
        <div className="desktop-icon" onDoubleClick={() => openWindow('filebrowser', 'File Browser')}>
          <div className="icon">📁</div>
          <div className="icon-label">My Files</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('recyclebin', 'Recycle Bin')}>
          <div className="icon">🗑️</div>
          <div className="icon-label">Recycle Bin</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('calculator', 'Calculator')}>
          <div className="icon">🔢</div>
          <div className="icon-label">Calculator</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('texteditor', 'Notepad')}>
          <div className="icon">📝</div>
          <div className="icon-label">Notepad</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('paint', 'Paint')}>
          <div className="icon">🎨</div>
          <div className="icon-label">Paint</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('webbrowser', 'Web Browser')}>
          <div className="icon">🌐</div>
          <div className="icon-label">Browser</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('games', 'Games')}>
          <div className="icon">🎮</div>
          <div className="icon-label">Games</div>
        </div>
        <div className="desktop-icon" onDoubleClick={() => openWindow('taskmanager', 'Task Manager')}>
          <div className="icon">📊</div>
          <div className="icon-label">Task Manager</div>
        </div>
      </div>

      {/* Windows */}
      {windows.filter(w => !w.isMinimized).map(window => (
        <Window
          key={window.id}
          id={window.id}
          title={window.title}
          onClose={closeWindow}
          onMinimize={minimizeWindow}
          onMaximize={maximizeWindow}
          isMaximized={window.isMaximized}
          zIndex={window.zIndex}
          onFocus={bringToFront}
        >
          {renderWindowContent(window)}
        </Window>
      ))}

      {/* Start Menu */}
      <StartMenu
        isOpen={startMenuOpen}
        onClose={() => setStartMenuOpen(false)}
        onOpenApp={openWindow}
        onShutDown={shutDown}
        user={user}
      />

      {/* Context Menu */}
      <ContextMenu
        isOpen={contextMenuOpen}
        position={contextMenuPosition}
        onClose={() => setContextMenuOpen(false)}
        onAction={handleContextAction}
      />

      {/* Taskbar */}
      <div className="taskbar" style={getTaskbarStyle()}>
        <button 
          className="start-btn"
          onClick={(e) => {
            e.stopPropagation();
            setStartMenuOpen(!startMenuOpen);
            setContextMenuOpen(false);
          }}
        >
          🏠 Start
        </button>
        
        <div className="taskbar-items">
          {windows.map(window => (
            <button
              key={window.id}
              className={`taskbar-item ${window.isMinimized ? 'minimized' : ''}`}
              onClick={() => window.isMinimized ? restoreWindow(window.id) : bringToFront(window.id)}
              title={window.title}
            >
              {window.title}
            </button>
          ))}
        </div>
        
        <div className="system-tray">
          <div className="tray-icons">
            <span className="tray-icon" title="Volume">🔊</span>
            <span className="tray-icon" title="Network">📶</span>
          </div>
          <span className="time">{currentTime.toLocaleTimeString()}</span>
          <button className="logout-btn" onClick={onLogout} title="Logout">Logout</button>
        </div>
      </div>
    </div>
  );
};

// Main App Component
function App() {
  const [user, setUser] = useState(null);

  const handleLogin = (userData) => {
    setUser(userData);
  };

  const handleLogout = () => {
    setUser(null);
  };

  return (
    <div className="App">
      {user ? (
        <Desktop user={user} onLogout={handleLogout} />
      ) : (
        <Login onLogin={handleLogin} />
      )}
    </div>
  );
}

export default App;