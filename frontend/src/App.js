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
      onClick={() => onFocus && onFocus(id)}
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

// Text Editor App (keeping original with small enhancements)
const TextEditor = ({ user, onSave }) => {
  const [content, setContent] = useState('');
  const [fileName, setFileName] = useState('Untitled.txt');
  const [isSaved, setIsSaved] = useState(true);

  const handleContentChange = (e) => {
    setContent(e.target.value);
    setIsSaved(false);
  };

  const saveFile = async () => {
    try {
      await axios.post(`${API}/files/${user.user_id}`, {
        name: fileName,
        type: 'file',
        content: content,
        path: `/Desktop/${fileName}`
      });
      setIsSaved(true);
      onSave && onSave();
    } catch (error) {
      console.error('Error saving file:', error);
    }
  };

  return (
    <div className="text-editor">
      <div className="text-editor-toolbar">
        <input 
          type="text" 
          value={fileName} 
          onChange={(e) => setFileName(e.target.value)}
          className="filename-input"
        />
        <button onClick={saveFile} className="save-btn" disabled={isSaved}>
          {isSaved ? 'Saved' : 'Save'}
        </button>
      </div>
      <textarea
        value={content}
        onChange={handleContentChange}
        className="text-editor-content"
        placeholder="Start typing..."
      />
    </div>
  );
};

// File Browser App (enhanced with better icons)
const FileBrowser = ({ user, onRefresh }) => {
  const [files, setFiles] = useState([]);
  const [currentPath, setCurrentPath] = useState('/');
  const [loading, setLoading] = useState(false);

  const loadFiles = async (path = '/') => {
    setLoading(true);
    try {
      const response = await axios.get(`${API}/files/${user.user_id}?path=${path}`);
      setFiles(response.data);
      setCurrentPath(path);
    } catch (error) {
      console.error('Error loading files:', error);
    }
    setLoading(false);
  };

  useEffect(() => {
    if (user) {
      loadFiles();
    }
  }, [user]);

  const createFolder = async () => {
    const folderName = prompt('Enter folder name:');
    if (folderName) {
      try {
        await axios.post(`${API}/files/${user.user_id}`, {
          name: folderName,
          type: 'folder',
          path: `${currentPath}${folderName}`
        });
        loadFiles(currentPath);
      } catch (error) {
        console.error('Error creating folder:', error);
      }
    }
  };

  const deleteFile = async (fileId) => {
    if (window.confirm('Are you sure you want to move this file to Recycle Bin?')) {
      try {
        await axios.delete(`${API}/files/${user.user_id}/${fileId}`);
        loadFiles(currentPath);
      } catch (error) {
        console.error('Error deleting file:', error);
      }
    }
  };

  return (
    <div className="file-browser">
      <div className="file-browser-toolbar">
        <div className="path-display">Path: {currentPath}</div>
        <button onClick={createFolder} className="new-folder-btn">New Folder</button>
      </div>
      <div className="file-list">
        {loading ? (
          <div>Loading...</div>
        ) : (
          files.map(file => (
            <div key={file.id} className="file-item">
              <div className="file-icon">
                {file.type === 'folder' ? '📁' : getFileIcon(file.file_extension)}
              </div>
              <div className="file-info">
                <div className="file-name">{file.name}</div>
                <div className="file-details">
                  {file.type} - {file.size} bytes
                </div>
              </div>
              <button 
                onClick={() => deleteFile(file.id)} 
                className="delete-btn"
                title="Move to Recycle Bin"
              >
                🗑️
              </button>
            </div>
          ))
        )}
      </div>
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

// Games Launcher App (keeping original)
const GamesLauncher = ({ onOpenGame }) => {
  const games = [
    { name: 'Solitaire', url: 'https://www.solitr.com/' },
    { name: 'Minesweeper', url: 'https://minesweeper.online/' },
    { name: 'Tetris', url: 'https://tetris.com/play-tetris' },
    { name: 'Snake', url: 'https://playsnake.org/' },
    { name: 'Pac-Man', url: 'https://pacman.live/' }
  ];

  return (
    <div className="games-launcher">
      <h3>Games</h3>
      <div className="games-grid">
        {games.map((game, index) => (
          <div 
            key={index} 
            className="game-tile"
            onClick={() => onOpenGame(game)}
          >
            <div className="game-icon">🎮</div>
            <div className="game-name">{game.name}</div>
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
        return <TextEditor user={user} />;
      case 'filebrowser':
        return <FileBrowser user={user} />;
      case 'webbrowser':
        return <WebBrowser />;
      case 'games':
        return <GamesLauncher onOpenGame={(game) => openWindow('game', game.name, { game })} />;
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